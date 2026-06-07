/**
 * Minimal fire-and-forget Discord webhook poster, built on Steam's ISteamHTTP.
 * The request frees itself on completion.
 */

#pragma once

#include <string>

// POST a single embed to the given Discord webhook URL.
// No-op if url is empty or Steam HTTP is unavailable.
void Discord_PostEmbed(const std::string &url, const std::string &title, const std::string &description, int color);
