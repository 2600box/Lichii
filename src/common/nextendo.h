// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// Nextendo Network sign-in and the linked account, ported from citron-nextendo's
// src/common/nextendo_account.cpp and src/web_service/nextendo_api.cpp (SignInWithBrowser).
namespace Common::Nextendo {

constexpr std::string_view ApiBase = "https://nextendo.network";
constexpr std::string_view AuthorizePath = "/oauth/authorize";
constexpr std::string_view TokenPath = "/api/oauth/token";
constexpr std::string_view UserInfoPath = "/api/me";
constexpr std::string_view Scopes = "identity friends";
constexpr u16 CallbackPort = 47823;

struct Account {
    u64 pid{};
    std::string username;
    std::string friend_code;
    // The "nx2." token the NEX game servers read from the id_token's "nnex" claim.
    std::string nex_token;
    std::string access_token;
    std::string refresh_token;
};

std::optional<Account> LoadAccount();
void SaveAccount(const Account& account);
void ClearAccount();

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
    u64 pid{};
    std::string username;
    std::string friend_code;
};

std::optional<TokenResponse> ParseTokenResponse(std::string_view body);

struct SignInResult {
    bool ok{};
    std::string error;
    std::vector<std::string> token_fields;
    Account account;
};

// Opens the authorize page through open_url, waits up to five minutes for the loopback
// redirect and exchanges the code for tokens.
SignInResult SignIn(std::string_view client_id,
                    const std::function<void(const std::string&)>& open_url);

} // namespace Common::Nextendo
