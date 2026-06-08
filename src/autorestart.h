/**
 * AutoRestart - Metamod:Source 2.0 plugin
 *
 * Restarts the CS2 server when a game/plugin update is detected
 * (via cs2docker's /watchdog version files)
 * or at a configured daily time. Meant to be used with cs2docker.
 */

#pragma once

#include <ISmmPlugin.h>
#include <sh_vector.h>

#include <eiface.h>
#include <iserver.h>
#include <playerslot.h>
#include "networksystem/inetworkserializer.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#ifndef ABSOLUTE_PLAYER_LIMIT
#define ABSOLUTE_PLAYER_LIMIT 64
#endif

class AutoRestartPlugin : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
	bool Unload(char *error, size_t maxlen) override;

public: // hooks
	void Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick);
	void Hook_StartupServer(const GameSessionConfiguration_t &config, ISource2WorldSession *, const char *);
	void Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 xuid, const char *pszNetworkID);
	void Hook_ServerHibernationUpdate(bool bHibernating);

public: // ISmmPlugin metadata
	const char *GetAuthor() override
	{
		return "jvnipers";
	}

	const char *GetName() override
	{
		return "Auto Restart";
	}

	const char *GetDescription() override
	{
		return "Auto restart the server when a game/plugin update is detected, or at a configured daily time. Meant to be used with cs2docker.";
	}

	const char *GetURL() override
	{
		return "https://github.com/FemboyKZ/cs2docker-autorestart";
	}

	const char *GetLicense() override
	{
		return "AGPL-3.0";
	}

	const char *GetVersion() override
	{
		return "1.3.0";
	}

	const char *GetDate() override
	{
		return __DATE__;
	}

	const char *GetLogTag() override
	{
		return "AutoRestart";
	}

private:
	// Ported from the C# OnTimerCallback / OnMapEnd logic.
	void CheckAndRestart();
	bool IsServerOutOfDate();
	bool CheckDailyRestart();
	int CountHumanPlayers() const;
	void PrintToChatAll(const char *msg);

	std::map<std::string, std::string> ReadPluginVersions() const;

	// True if the version file at path is unchanged (by mtime) since last checked;
	// records the current mtime as a side effect.
	// Lets IsServerOutOfDate() skip re-reading files that haven't moved.
	bool VersionFileUnchanged(const std::string &path);

	// While the server hibernates GameFrame is frozen, so the normal restart path can't run.
	// This thread polls the version files and, if an update lands while hibernating,
	// signals the process to shut down for relaunch.
	void WatcherLoop();

	// Thread-safe out-of-date check: reads only the immutable startup snapshot
	// (m_buildVersion, m_pluginVersions) plus the files, never the mtime cache.
	bool IsOutOfDateSnapshot() const;

	std::string m_buildVersion;
	std::map<std::string, std::string> m_pluginVersions; // snapshot taken at load

	std::string m_discordWebhook;   // optional Discord webhook URL (env: discord_webhook)
	std::string m_serverName;       // optional server name for notifications (env: server_name)
	bool m_discordNotified = false; // ensures we post to Discord only once per restart decision

	bool m_restartNeeded = false;
	bool m_scheduledRestartNeeded = false;

	bool m_outOfDate = false;
	double m_lastVersionCheckTime = 0.0;                                    // Plat_FloatTime() of last version-file poll
	std::map<std::string, std::filesystem::file_time_type> m_versionMtimes; // path -> last-seen mtime

	bool m_hasDailyRestart = false;
	int m_dailyRestartSeconds = 0;  // seconds since UTC midnight
	int m_lastDailyRestartDay = -1; // days since unix epoch (UTC) of last daily restart

	double m_lastCheckTime = 0.0; // Plat_FloatTime() of last 10s tick
	int m_startupCount = 0;       // number of StartupServer calls seen (first == initial boot map)

	// Empty-server quit is delayed so the async Discord webhook has time to flush.
	bool m_quitPending = false;
	double m_quitAtTime = 0.0; // Plat_FloatTime() at which to issue the deferred quit

	// Background watcher state. m_hibernating is the engine's hibernation signal
	// (set from Hook_ServerHibernationUpdate); the thread only acts while it's true.
	std::atomic<bool> m_hibernating {false};
	std::atomic<bool> m_stopWatcher {false};
	std::thread m_watcherThread;
	std::mutex m_watcherMutex;
	std::condition_variable m_watcherCv;
};

extern AutoRestartPlugin g_AutoRestartPlugin;

PLUGIN_GLOBALVARS();
