// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace httplib {
class Client;
}

// Nextendo Network sign-in and the linked account, ported from citron-nextendo's
// src/common/nextendo_account.cpp and src/web_service/nextendo_api.cpp (SignInWithBrowser).
namespace Common::Nextendo {

constexpr std::string_view ApiBase = "https://nextendo.network";
constexpr std::string_view AuthorizePath = "/oauth/authorize";
constexpr std::string_view TokenPath = "/api/oauth/token";
constexpr std::string_view UserInfoPath = "/api/me";
// The game token for third-party clients (wiki.nextendo.network/developers, "Game login").
constexpr std::string_view NexTokenPath = "/api/nex-token";
constexpr std::string_view Scopes = "identity game.matchmaking";
constexpr u16 CallbackPort = 47823;

// Online play through Nextendo (host redirects, SNI recovery, the BAAS id_token and the license
// kind) is built but held back for now; set this to true to offer it. Sign-in works either way.
constexpr bool OnlinePlayAvailable = false;

// True when online play is available and the user turned it on.
bool OnlinePlayActive();

struct Account {
    u64 pid{};
    std::string username;
    std::string friend_code;
    // The "nx2." token the NEX game servers read from the id_token's "nnex" claim.
    std::string nex_token;
    std::string access_token;
    std::string refresh_token;
    // Unix times; the access token lasts an hour and the game token a day.
    s64 access_expires{};
    s64 nex_expires{};
};

std::optional<Account> LoadAccount();
void SaveAccount(const Account& account);
void ClearAccount();

// Timeouts, redirects and, with the bundled OpenSSL, the CA store for nextendo.network.
void ConfigureClient(httplib::Client& client, time_t timeout_seconds);

std::string Base64UrlEncode(std::span<const u8> data);

struct Pkce {
    std::string verifier;
    std::string challenge;
};

// RFC 7636 S256: BASE64URL(SHA256(verifier)) without padding.
std::string PkceChallenge(std::string_view verifier);
Pkce MakePkce();

std::string RedirectUri();
std::string BuildAuthorizeUrl(std::string_view client_id, std::string_view state,
                              std::string_view challenge);

struct Callback {
    std::string code;
    std::string error;
};

// Checks the loopback redirect's query against the state the authorize request carried.
Callback ParseCallback(const std::multimap<std::string, std::string>& params,
                       std::string_view expected_state);

struct TokenResponse {
    // Top-level field names, so a sign-in can report what the server sent without the values.
    std::vector<std::string> fields;
    std::string nex_token;
    std::string access_token;
    std::string refresh_token;
    s64 expires_in{};
    u64 pid{};
    std::string username;
    std::string friend_code;
};

// Reads both the OAuth token response and the /api/nex-token response.
std::optional<TokenResponse> ParseTokenResponse(std::string_view body);

struct SignInResult {
    bool ok{};
    std::string error;
    std::vector<std::string> token_fields;
    // Set when sign-in worked but no game token came back, so games can't log in.
    std::string game_token_error;
    Account account;
};

// Opens the authorize page through open_url, waits up to five minutes for the loopback
// redirect and exchanges the code for tokens.
SignInResult SignIn(std::string_view client_id,
                    const std::function<void(const std::string&)>& open_url);

// The stored account with a game token that is still valid, renewing the access token and the
// game token when they have expired. Keeps the stored tokens when nextendo.network can't be reached.
std::optional<Account> EnsureGameToken(std::string_view client_id);

} // namespace Common::Nextendo
