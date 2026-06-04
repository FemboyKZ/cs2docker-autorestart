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

#include <map>
#include <string>

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
	void Hook_OnClientConnected(CPlayerSlot slot, const char *pszName, uint64 xuid, const char *pszNetworkID, const char *pszAddress,
								bool bFakePlayer);
	void Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 xuid, const char *pszNetworkID);

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
		return "1.1.1";
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

	std::string m_buildVersion;
	std::map<std::string, std::string> m_pluginVersions; // snapshot taken at load

	std::string m_discordWebhook;   // optional Discord webhook URL (env: discord_webhook)
	bool m_discordNotified = false; // ensures we post to Discord only once per restart decision

	bool m_restartNeeded = false;
	bool m_scheduledRestartNeeded = false;

	bool m_hasDailyRestart = false;
	int m_dailyRestartSeconds = 0;  // seconds since UTC midnight
	int m_lastDailyRestartDay = -1; // days since unix epoch (UTC) of last daily restart

	double m_lastCheckTime = 0.0; // Plat_FloatTime() of last 10s tick
	int m_startupCount = 0;       // number of StartupServer calls seen (first == initial boot map)

	// Empty-server quit is delayed so the async Discord webhook has time to flush.
	bool m_quitPending = false;
	double m_quitAtTime = 0.0; // Plat_FloatTime() at which to issue the deferred quit

	bool m_humanConnected[ABSOLUTE_PLAYER_LIMIT] = {};
};

extern AutoRestartPlugin g_AutoRestartPlugin;

PLUGIN_GLOBALVARS();
