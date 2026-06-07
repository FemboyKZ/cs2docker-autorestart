#include "autorestart.h"
#include "discord.h"
#include "recipientfilter.h"

#include "engine/igameeventsystem.h"
#include "interfaces/interfaces.h" // provides g_pSource2Server, g_pSource2GameClients, g_pNetworkServerService, g_pNetworkMessages
#include "networksystem/inetworkmessages.h"
#include "networksystem/netmessage.h"

#include "tier0/dbg.h"
#include "tier0/platform.h"

#include "usermessages.pb.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

#include "tier0/memdbgon.h"

// HUD destination for chat text (HUD_PRINTTALK). Not exported by a public header.
#define HUD_PRINTTALK 3

AutoRestartPlugin g_AutoRestartPlugin;

IVEngineServer2 *g_pEngineServer2 = nullptr;
IGameEventSystem *g_gameEventSystem = nullptr;

PLUGIN_EXPOSE(AutoRestartPlugin, g_AutoRestartPlugin);

class GameSessionConfiguration_t
{
};

SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);
SH_DECL_HOOK3_void(INetworkServerService, StartupServer, SH_NOATTRIB, 0, const GameSessionConfiguration_t &, ISource2WorldSession *, const char *);

static const char *kBuildVersionFile = "/watchdog/cs2/latest.txt";
static const char *kLayersDir = "/watchdog/layers";
static const double kVersionCheckInterval = 60.0; // seconds

static std::string Trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
	{
		return "";
	}
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

static std::string ReadFileTrimmed(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		return "";
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	return Trim(ss.str());
}

bool AutoRestartPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer2, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, SOURCE2GAMECLIENTS_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_gameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);

	const char *buildVer = std::getenv("build_ver");
	if (!buildVer || !*buildVer)
	{
		V_snprintf(error, maxlen, "Environment variable 'build_ver' was not found, this plugin is meant to be used with cs2docker!");
		return false;
	}
	m_buildVersion = Trim(buildVer);

	// Snapshot current plugin versions from the watchdog layer latest.txt files.
	m_pluginVersions = ReadPluginVersions();

	// Parse optional daily restart time (UTC, "HH:mm" or "HH:mm:ss").
	const char *dailyStr = std::getenv("daily_restart_time");
	if (dailyStr && *dailyStr)
	{
		int hh = 0, mm = 0, ss = 0;
		int n = std::sscanf(Trim(dailyStr).c_str(), "%d:%d:%d", &hh, &mm, &ss);
		if (n >= 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60 && ss >= 0 && ss < 60)
		{
			m_dailyRestartSeconds = hh * 3600 + mm * 60 + ss;
			m_hasDailyRestart = true;

			std::time_t now = std::time(nullptr);
			int today = static_cast<int>(now / 86400);
			int secOfDay = static_cast<int>(now % 86400);
			m_lastDailyRestartDay = (secOfDay >= m_dailyRestartSeconds) ? today : today - 1;
		}
	}

	// Optional Discord webhook for restart notifications.
	const char *webhook = std::getenv("discord_webhook");
	if (webhook && *webhook)
	{
		m_discordWebhook = Trim(webhook);
	}

	// Optional server name shown in the Discord notification.
	const char *serverName = std::getenv("server_name");
	if (serverName && *serverName)
	{
		m_serverName = Trim(serverName);
	}

	// On a late load the boot map's StartupServer already fired
	// count it as seen so the next map change isn't mistaken for the initial boot.
	if (late)
	{
		m_startupCount = 1;
		Msg("[AutoRestart] Late load detected; %d player(s) currently connected.\n", CountHumanPlayers());
	}

	SH_ADD_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &AutoRestartPlugin::Hook_GameFrame), true);
	SH_ADD_HOOK(INetworkServerService, StartupServer, g_pNetworkServerService, SH_MEMBER(this, &AutoRestartPlugin::Hook_StartupServer), true);

	// 0.0 forces a check on the first frame
	m_lastCheckTime = 0.0;
	m_lastVersionCheckTime = -kVersionCheckInterval;

	Msg("[AutoRestart] Loaded. build_ver=%s, daily_restart=%s, discord=%s\n", m_buildVersion.c_str(), m_hasDailyRestart ? "on" : "off",
		m_discordWebhook.empty() ? "off" : "on");

	return true;
}

bool AutoRestartPlugin::Unload(char *error, size_t maxlen)
{
	SH_REMOVE_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &AutoRestartPlugin::Hook_GameFrame), true);
	SH_REMOVE_HOOK(INetworkServerService, StartupServer, g_pNetworkServerService, SH_MEMBER(this, &AutoRestartPlugin::Hook_StartupServer), true);
	return true;
}

std::map<std::string, std::string> AutoRestartPlugin::ReadPluginVersions() const
{
	std::map<std::string, std::string> versions;
	std::error_code ec;
	if (!fs::is_directory(kLayersDir, ec))
	{
		return versions;
	}

	for (const auto &entry : fs::directory_iterator(kLayersDir, ec))
	{
		if (!entry.is_directory(ec))
		{
			continue;
		}
		std::string latest = ReadFileTrimmed((entry.path() / "latest.txt").string());
		if (!latest.empty())
		{
			versions[entry.path().filename().string()] = latest;
		}
	}

	return versions;
}

bool AutoRestartPlugin::VersionFileUnchanged(const std::string &path)
{
	std::error_code ec;
	auto mt = fs::last_write_time(path, ec);
	if (ec)
	{
		// Can't stat
		return false;
	}
	auto it = m_versionMtimes.find(path);
	if (it != m_versionMtimes.end() && it->second == mt)
	{
		return true;
	}
	m_versionMtimes[path] = mt;
	return false;
}

bool AutoRestartPlugin::IsServerOutOfDate()
{
	// Build version: only re-read when the file's mtime has moved.
	if (!VersionFileUnchanged(kBuildVersionFile))
	{
		std::string latestBuild = ReadFileTrimmed(kBuildVersionFile);
		if (!latestBuild.empty() && m_buildVersion != latestBuild)
		{
			return true;
		}
	}

	// Plugin layers: iterate the names captured at startup and
	// skip any whose latest.txt mtime is unchanged since we last looked.
	for (const auto &[name, startupVer] : m_pluginVersions)
	{
		std::string path = std::string(kLayersDir) + "/" + name + "/latest.txt";
		if (VersionFileUnchanged(path))
		{
			continue;
		}
		std::string current = ReadFileTrimmed(path);
		if (!current.empty() && current != startupVer)
		{
			return true;
		}
	}
	return false;
}

bool AutoRestartPlugin::CheckDailyRestart()
{
	if (!m_hasDailyRestart || m_scheduledRestartNeeded)
	{
		return false;
	}

	std::time_t now = std::time(nullptr);
	int today = static_cast<int>(now / 86400);    // days since epoch (UTC)
	int secOfDay = static_cast<int>(now % 86400); // seconds since UTC midnight

	return today > m_lastDailyRestartDay && secOfDay >= m_dailyRestartSeconds;
}

int AutoRestartPlugin::CountHumanPlayers() const
{
	int count = 0;
	for (int i = 0; i < ABSOLUTE_PLAYER_LIMIT; i++)
	{
		if (g_pEngineServer2->GetPlayerNetInfo(CPlayerSlot(i)) != nullptr)
		{
			count++;
		}
	}
	return count;
}

void AutoRestartPlugin::PrintToChatAll(const char *msg)
{
	INetworkMessageInternal *pNetMsg = g_pNetworkMessages->FindNetworkMessagePartial("TextMsg");
	if (!pNetMsg)
	{
		return;
	}

	auto data = pNetMsg->AllocateMessage()->ToPB<CUserMessageTextMsg>();
	data->set_dest(HUD_PRINTTALK);
	data->add_param(msg);

	CSimpleRecipientFilter filter;
	for (int i = 0; i < ABSOLUTE_PLAYER_LIMIT; i++)
	{
		if (g_pEngineServer2->GetPlayerNetInfo(CPlayerSlot(i)) != nullptr)
		{
			filter.AddRecipient(i);
		}
	}

	g_gameEventSystem->PostEventAbstract(-1, false, &filter, pNetMsg, data, 0);

	delete data;
}

void AutoRestartPlugin::CheckAndRestart()
{
	bool isDailyRestartDue = CheckDailyRestart();

	if (!m_outOfDate)
	{
		double now = Plat_FloatTime();
		if (now - m_lastVersionCheckTime >= kVersionCheckInterval)
		{
			m_lastVersionCheckTime = now;
			m_outOfDate = IsServerOutOfDate();
		}
	}

	if (!(isDailyRestartDue || m_scheduledRestartNeeded || m_outOfDate))
	{
		return;
	}

	if (isDailyRestartDue && !m_scheduledRestartNeeded)
	{
		m_scheduledRestartNeeded = true;
		m_lastDailyRestartDay = static_cast<int>(std::time(nullptr) / 86400);
	}

	int numPlayers = CountHumanPlayers();

	bool scheduled = isDailyRestartDue || m_scheduledRestartNeeded;
	const char *reason = scheduled ? "Scheduled daily restart" : "Server update";

	// Notify Discord once per restart decision.
	if (!m_discordNotified && !m_discordWebhook.empty())
	{
		m_discordNotified = true;
		int color = scheduled ? 0x3498DB : 0xE67E22; // blue for daily, orange for update
		char desc[256];
		if (numPlayers == 0)
		{
			V_snprintf(desc, sizeof(desc), "%s - server empty, restarting now.", reason);
		}
		else
		{
			V_snprintf(desc, sizeof(desc), "%s - %d player%s online, restarting at next map.", reason, numPlayers, numPlayers == 1 ? "" : "s");
		}
		const char *title = m_serverName.empty() ? "AutoRestart" : m_serverName.c_str();
		Discord_PostEmbed(m_discordWebhook, title, desc, color);
	}

	if (numPlayers == 0)
	{
		if (!m_quitPending)
		{
			double delay = m_discordWebhook.empty() ? 0.0 : 5.0;
			m_quitPending = true;
			m_quitAtTime = Plat_FloatTime() + delay;
			Msg("[AutoRestart] %s: server empty, quitting in %.0fs.\n", reason, delay);
		}
	}
	else if (!m_restartNeeded)
	{
		m_restartNeeded = true;
		Msg("[AutoRestart] %s: %d player(s) online, will restart at next map.\n", reason, numPlayers);
		PrintToChatAll("The server will restart at the next opportunity!");
	}
}

void AutoRestartPlugin::Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick)
{
	double now = Plat_FloatTime();

	if (m_quitPending && now >= m_quitAtTime)
	{
		Msg("[AutoRestart] Deferred quit firing, shutting down server.\n");
		g_pEngineServer2->ServerCommand("quit");
		RETURN_META(MRES_IGNORED);
	}

	if (now - m_lastCheckTime < 10.0)
	{
		RETURN_META(MRES_IGNORED);
	}

	m_lastCheckTime = now;
	CheckAndRestart();

	RETURN_META(MRES_IGNORED);
}

void AutoRestartPlugin::Hook_StartupServer(const GameSessionConfiguration_t &config, ISource2WorldSession *, const char *)
{
	// The first StartupServer call is the initial boot map; ignore it so we don't
	// quit immediately. Subsequent calls are map changes.
	m_startupCount++;
	if (m_startupCount <= 1)
	{
		RETURN_META(MRES_IGNORED);
	}

	if (!m_outOfDate)
	{
		m_outOfDate = IsServerOutOfDate();
	}

	if (m_restartNeeded || m_scheduledRestartNeeded || m_outOfDate)
	{
		Msg("[AutoRestart] Map change with restart pending, shutting down server.\n");
		g_pEngineServer2->ServerCommand("quit");
	}

	RETURN_META(MRES_IGNORED);
}
