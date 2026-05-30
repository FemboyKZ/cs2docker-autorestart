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

// Directory iteration: POSIX dirent on Linux (std::filesystem drags in libstdc++'s
// <ranges>, which clang rejects on the steamrt toolchain), std::filesystem elsewhere.
#ifdef _WIN32
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

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
SH_DECL_HOOK6_void(IServerGameClients, OnClientConnected, SH_NOATTRIB, 0, CPlayerSlot, const char *, uint64, const char *, const char *, bool);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64,
				   const char *);

static const char *kBuildVersionFile = "/watchdog/cs2/latest.txt";
static const char *kLayersDir = "/watchdog/layers";

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
		}
	}

	// Optional Discord webhook for restart notifications.
	const char *webhook = std::getenv("discord_webhook");
	if (webhook && *webhook)
	{
		m_discordWebhook = Trim(webhook);
	}

	SH_ADD_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &AutoRestartPlugin::Hook_GameFrame), true);
	SH_ADD_HOOK(INetworkServerService, StartupServer, g_pNetworkServerService, SH_MEMBER(this, &AutoRestartPlugin::Hook_StartupServer), true);
	SH_ADD_HOOK(IServerGameClients, OnClientConnected, g_pSource2GameClients, SH_MEMBER(this, &AutoRestartPlugin::Hook_OnClientConnected), false);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &AutoRestartPlugin::Hook_ClientDisconnect), true);

	// 0.0 forces a check on the first frame.
	m_lastCheckTime = 0.0;

	Msg("[AutoRestart] Loaded. build_ver=%s, daily_restart=%s, discord=%s\n", m_buildVersion.c_str(), m_hasDailyRestart ? "on" : "off",
		m_discordWebhook.empty() ? "off" : "on");

	return true;
}

bool AutoRestartPlugin::Unload(char *error, size_t maxlen)
{
	SH_REMOVE_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &AutoRestartPlugin::Hook_GameFrame), true);
	SH_REMOVE_HOOK(INetworkServerService, StartupServer, g_pNetworkServerService, SH_MEMBER(this, &AutoRestartPlugin::Hook_StartupServer), true);
	SH_REMOVE_HOOK(IServerGameClients, OnClientConnected, g_pSource2GameClients, SH_MEMBER(this, &AutoRestartPlugin::Hook_OnClientConnected), false);
	SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &AutoRestartPlugin::Hook_ClientDisconnect), true);
	return true;
}

std::map<std::string, std::string> AutoRestartPlugin::ReadPluginVersions() const
{
	std::map<std::string, std::string> versions;

#ifdef _WIN32
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
#else
	DIR *dir = opendir(kLayersDir);
	if (!dir)
	{
		return versions;
	}

	while (struct dirent *ent = readdir(dir))
	{
		std::string name = ent->d_name;
		if (name == "." || name == "..")
		{
			continue;
		}

		std::string full = std::string(kLayersDir) + "/" + name;
		struct stat st;
		if (stat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
		{
			continue;
		}

		std::string latest = ReadFileTrimmed(full + "/latest.txt");
		if (!latest.empty())
		{
			versions[name] = latest;
		}
	}
	closedir(dir);
#endif

	return versions;
}

bool AutoRestartPlugin::IsServerOutOfDate()
{
	std::string latestBuild = ReadFileTrimmed(kBuildVersionFile);
	if (!latestBuild.empty() && m_buildVersion != latestBuild)
	{
		return true;
	}

	// Check if any plugin layer has been updated since startup.
	std::map<std::string, std::string> current = ReadPluginVersions();
	for (const auto &[name, startupVer] : m_pluginVersions)
	{
		auto it = current.find(name);
		if (it != current.end() && it->second != startupVer)
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
		if (m_humanConnected[i])
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
		if (m_humanConnected[i])
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

	if (!(isDailyRestartDue || m_scheduledRestartNeeded || IsServerOutOfDate()))
	{
		return;
	}

	if (isDailyRestartDue && !m_scheduledRestartNeeded)
	{
		m_scheduledRestartNeeded = true;
		m_lastDailyRestartDay = static_cast<int>(std::time(nullptr) / 86400);
	}

	int numPlayers = CountHumanPlayers();

	// Notify Discord once per restart decision.
	if (!m_discordNotified && !m_discordWebhook.empty())
	{
		m_discordNotified = true;
		const char *reason = (isDailyRestartDue || m_scheduledRestartNeeded) ? "scheduled daily restart" : "server update";
		char buf[256];
		if (numPlayers == 0)
		{
			V_snprintf(buf, sizeof(buf), "AutoRestart: %s - server empty, restarting now.", reason);
		}
		else
		{
			V_snprintf(buf, sizeof(buf), "AutoRestart: %s - %d player%s online, restarting at next map.", reason, numPlayers,
					   numPlayers == 1 ? "" : "s");
		}
		Discord_PostWebhook(m_discordWebhook, buf);
	}

	if (numPlayers == 0)
	{
		g_pEngineServer2->ServerCommand("quit");
	}
	else if (!m_restartNeeded)
	{
		m_restartNeeded = true;
		PrintToChatAll("The server will restart at the next opportunity!");
	}
}

void AutoRestartPlugin::Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick)
{
	double now = Plat_FloatTime();
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

	if (m_restartNeeded || m_scheduledRestartNeeded || IsServerOutOfDate())
	{
		g_pEngineServer2->ServerCommand("quit");
	}

	RETURN_META(MRES_IGNORED);
}

void AutoRestartPlugin::Hook_OnClientConnected(CPlayerSlot slot, const char *pszName, uint64 xuid, const char *pszNetworkID, const char *pszAddress,
											   bool bFakePlayer)
{
	int s = slot.Get();
	if (s >= 0 && s < ABSOLUTE_PLAYER_LIMIT)
	{
		m_humanConnected[s] = !bFakePlayer;
	}

	RETURN_META(MRES_IGNORED);
}

void AutoRestartPlugin::Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 xuid,
											  const char *pszNetworkID)
{
	int s = slot.Get();
	if (s >= 0 && s < ABSOLUTE_PLAYER_LIMIT)
	{
		m_humanConnected[s] = false;
	}

	RETURN_META(MRES_IGNORED);
}
