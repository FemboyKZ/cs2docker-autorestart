/**
 * Minimal fire-and-forget Discord webhook poster, built on Steam's ISteamHTTP.
 * The request frees itself on completion.
 */

#pragma once

#include <string>

// POST {"content": <content>} to the given Discord webhook URL.
// No-op if url is empty or Steam HTTP is unavailable.
void Discord_PostWebhook(const std::string &url, const std::string &content);
