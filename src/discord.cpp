/**
 * Minimal fire-and-forget Discord webhook poster, built on Steam's ISteamHTTP.
 */

#include "discord.h"

#include <eiface.h>
#include <steam/steam_gameserver.h>

#include <string>

#include "tier0/memdbgon.h"

// Defined in autorestart.cpp.
extern IVEngineServer2 *g_pEngineServer2;

static ISteamHTTP *GetSteamHTTP()
{
	// Dedicated servers use the gameserver Steam context.
	if (g_pEngineServer2 && !g_pEngineServer2->IsDedicatedServer())
	{
		return SteamHTTP();
	}
	return SteamGameServerHTTP();
}

// Escape a string for embedding inside a JSON string literal.
static std::string JsonEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 16);
	for (char c : s)
	{
		switch (c)
		{
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				if (static_cast<unsigned char>(c) < 0x20)
				{
					char buf[8];
					V_snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				}
				else
				{
					out += c;
				}
				break;
		}
	}
	return out;
}

// Self-deleting wrapper that releases the request handle once Steam reports it done.
class DiscordRequest
{
public:
	DiscordRequest(HTTPRequestHandle handle, SteamAPICall_t call) : m_handle(handle)
	{
		m_callResult.SetGameserverFlag();
		m_callResult.Set(call, this, &DiscordRequest::OnCompleted);
	}

private:
	void OnCompleted(HTTPRequestCompleted_t *arg, bool bFailed)
	{
		if (ISteamHTTP *http = GetSteamHTTP())
		{
			http->ReleaseHTTPRequest(arg->m_hRequest);
		}
		delete this;
	}

	HTTPRequestHandle m_handle;
	CCallResult<DiscordRequest, HTTPRequestCompleted_t> m_callResult;
};

void Discord_PostWebhook(const std::string &url, const std::string &content)
{
	if (url.empty())
	{
		return;
	}

	ISteamHTTP *http = GetSteamHTTP();
	if (!http)
	{
		return;
	}

	std::string body = "{\"content\":\"" + JsonEscape(content) + "\"}";

	HTTPRequestHandle req = http->CreateHTTPRequest(k_EHTTPMethodPOST, url.c_str());
	if (req == INVALID_HTTPREQUEST_HANDLE)
	{
		return;
	}

	http->SetHTTPRequestRawPostBody(req, "application/json", (uint8 *)body.c_str(), body.length());

	SteamAPICall_t call;
	if (!http->SendHTTPRequest(req, &call))
	{
		http->ReleaseHTTPRequest(req);
		return;
	}

	new DiscordRequest(req, call);
}
