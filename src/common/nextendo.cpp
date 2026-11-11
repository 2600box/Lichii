// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/nextendo.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace Common::Nextendo {

namespace {

std::mutex account_mutex;
// Bumped by every sign-in and sign-out, so a renewal that started before one doesn't save over it.
u64 account_generation = 0;
std::mutex refresh_mutex;
s64 last_renewal_failure = 0;
std::optional<Account> cached_account;
bool account_loaded = false;

std::filesystem::path AccountPath() {
    return FS::GetEdenPath(FS::EdenPath::ConfigDir) / "nextendo_account.json";
}

std::string PercentEncode(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += c;
        } else {
            out += fmt::format("%{:02X}", u);
        }
    }
    return out;
}

std::string RandomToken() {
    std::array<u8, 32> raw{};
    RAND_bytes(raw.data(), static_cast<int>(raw.size()));
    return Base64UrlEncode(raw);
}

u64 ReadPid(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        return value.get<u64>();
    }
    if (value.is_string()) {
        try {
            return std::stoull(value.get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

// Server fields may be null or another type; those read as unset instead of throwing.
std::string StringField(const nlohmann::json& object, const char* key, std::string fallback = {}) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::move(fallback);
}

void ReadIdentity(const nlohmann::json& object, u64& pid, std::string& username,
                  std::string& friend_code) {
    if (!object.is_object()) {
        return;
    }
    if (const auto it = object.find("pid"); it != object.end()) {
        pid = ReadPid(*it);
    }
    username = StringField(object, "username", username);
    friend_code = StringField(object, "friend_code", friend_code);
}

std::string ServerError(const std::string& body) {
    try {
        const auto json = nlohmann::json::parse(body);
        for (const char* key : {"error_description", "message", "error"}) {
            if (const auto it = json.find(key); it != json.end() && it->is_string()) {
                return it->get<std::string>();
            }
        }
    } catch (...) {
    }
    return {};
}

s64 Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

s64 IntField(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<s64>() : 0;
}

void ConfigureClient(httplib::Client& client, time_t timeout_seconds) {
    client.set_connection_timeout(timeout_seconds);
    client.set_read_timeout(timeout_seconds);
    client.set_write_timeout(timeout_seconds);
    client.set_follow_location(true);
#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
}

httplib::Headers BaseHeaders(std::string_view client_id) {
    return {{"User-Agent", "Lichii"}, {"X-Nextendo-Client-Id", std::string{client_id}}};
}

std::optional<TokenResponse> ReadReply(const httplib::Result& result, std::string_view what,
                                       std::string& error) {
    if (!result) {
        error = "Could not reach nextendo.network.";
        LOG_ERROR(Common, "Nextendo {} failed: {}", what, httplib::to_string(result.error()));
        return std::nullopt;
    }
    if (result->status != 200) {
        const std::string reason = ServerError(result->body);
        error = fmt::format("nextendo.network refused the {} (HTTP {}){}{}", what, result->status,
                            reason.empty() ? "" : ": ", reason);
        LOG_ERROR(Common, "{}", error);
        return std::nullopt;
    }
    auto token = ParseTokenResponse(result->body);
    if (!token) {
        error = fmt::format("nextendo.network sent an unreadable {} response.", what);
        return std::nullopt;
    }
    LOG_INFO(Common, "Nextendo {} response fields: {}", what, fmt::join(token->fields, ", "));
    return token;
}

void ApplyIdentity(Account& account, const TokenResponse& token) {
    if (token.pid != 0) {
        account.pid = token.pid;
    }
    if (!token.username.empty()) {
        account.username = token.username;
    }
    if (!token.friend_code.empty()) {
        account.friend_code = token.friend_code;
    }
}

void ApplyTokens(Account& account, const TokenResponse& token) {
    account.access_token = token.access_token;
    account.access_expires = token.expires_in > 0 ? Now() + token.expires_in : 0;
    if (!token.refresh_token.empty()) {
        account.refresh_token = token.refresh_token;
    }
    if (!token.nex_token.empty()) {
        account.nex_token = token.nex_token;
        account.nex_expires = 0;
    }
    ApplyIdentity(account, token);
}

// Third-party clients trade the access token for the day-long nx2 game token that NEX servers
// accept (wiki.nextendo.network/developers, "Game login").
bool FetchGameToken(httplib::Client& client, std::string_view client_id, Account& account,
                    std::string& error) {
    auto headers = BaseHeaders(client_id);
    headers.emplace("Authorization", "Bearer " + account.access_token);
    const auto token = ReadReply(client.Get(std::string{NexTokenPath}, headers), "game token", error);
    if (!token) {
        return false;
    }
    if (token->nex_token.empty()) {
        error = "nextendo.network sent no game token.";
        return false;
    }
    account.nex_token = token->nex_token;
    account.nex_expires = Now() + (token->expires_in > 0 ? token->expires_in : 24 * 60 * 60);
    ApplyIdentity(account, *token);
    return true;
}

} // namespace

std::optional<Account> LoadAccount() {
    std::scoped_lock lock{account_mutex};
    if (account_loaded) {
        return cached_account;
    }
    account_loaded = true;
    const std::string text = FS::ReadStringFromFile(AccountPath(), FS::FileType::TextFile);
    if (text.empty()) {
        return cached_account;
    }
    try {
        const auto json = nlohmann::json::parse(text);
        Account account;
        ReadIdentity(json, account.pid, account.username, account.friend_code);
        account.nex_token = StringField(json, "nex_token");
        account.access_token = StringField(json, "access_token");
        account.refresh_token = StringField(json, "refresh_token");
        account.access_expires = IntField(json, "access_expires");
        account.nex_expires = IntField(json, "nex_expires");
        cached_account = std::move(account);
    } catch (const std::exception& e) {
        LOG_WARNING(Common, "Ignoring an unreadable Nextendo account file: {}", e.what());
    }
    return cached_account;
}

namespace {

void WriteAccountLocked(const Account& account) {
    const nlohmann::json json{
        {"pid", account.pid},
        {"username", account.username},
        {"friend_code", account.friend_code},
        {"nex_token", account.nex_token},
        {"access_token", account.access_token},
        {"refresh_token", account.refresh_token},
        {"access_expires", account.access_expires},
        {"nex_expires", account.nex_expires},
    };
    const auto path = AccountPath();
    void(FS::CreateParentDirs(path));
    if (FS::WriteStringToFile(path, FS::FileType::TextFile, json.dump(2)) == 0) {
        LOG_ERROR(Common, "Could not write the Nextendo account file");
    }
    cached_account = account;
    account_loaded = true;
}

bool SaveRenewedAccount(const Account& account, u64 generation) {
    std::scoped_lock lock{account_mutex};
    if (generation != account_generation) {
        return false;
    }
    WriteAccountLocked(account);
    return true;
}

} // namespace

void SaveAccount(const Account& account) {
    std::scoped_lock lock{account_mutex};
    ++account_generation;
    WriteAccountLocked(account);
}

void ClearAccount() {
    std::scoped_lock lock{account_mutex};
    ++account_generation;
    void(FS::RemoveFile(AccountPath()));
    cached_account.reset();
    account_loaded = true;
}

std::string Base64UrlEncode(std::span<const u8> data) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::size_t remaining = data.size() - i;
        const u32 triple = (u32{data[i]} << 16) | (remaining > 1 ? u32{data[i + 1]} << 8 : 0) |
                           (remaining > 2 ? u32{data[i + 2]} : 0);
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        if (remaining > 1) {
            out += alphabet[(triple >> 6) & 0x3F];
        }
        if (remaining > 2) {
            out += alphabet[triple & 0x3F];
        }
    }
    return out;
}

std::string PkceChallenge(std::string_view verifier) {
    std::array<u8, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(),
           digest.data());
    return Base64UrlEncode(digest);
}

Pkce MakePkce() {
    Pkce pkce{.verifier = RandomToken()};
    pkce.challenge = PkceChallenge(pkce.verifier);
    return pkce;
}

std::string RedirectUri() {
    return fmt::format("http://127.0.0.1:{}/callback", CallbackPort);
}

std::string BuildAuthorizeUrl(std::string_view client_id, std::string_view state,
                              std::string_view challenge) {
    return fmt::format("{}{}?response_type=code&client_id={}&redirect_uri={}&scope={}&state={}"
                       "&code_challenge={}&code_challenge_method=S256",
                       ApiBase, AuthorizePath, PercentEncode(client_id),
                       PercentEncode(RedirectUri()), PercentEncode(Scopes), PercentEncode(state),
                       PercentEncode(challenge));
}

Callback ParseCallback(const std::multimap<std::string, std::string>& params,
                       std::string_view expected_state) {
    const auto get = [&params](const char* key) -> std::string {
        const auto it = params.find(key);
        return it == params.end() ? std::string{} : it->second;
    };
    if (const std::string error = get("error"); !error.empty()) {
        const std::string description = get("error_description");
        return {.error = description.empty() ? error : fmt::format("{}: {}", error, description)};
    }
    if (get("state") != expected_state) {
        return {.error = "The sign-in reply did not match this request."};
    }
    std::string code = get("code");
    if (code.empty()) {
        return {.error = "The browser returned no authorization code."};
    }
    return {.code = std::move(code)};
}

std::optional<TokenResponse> ParseTokenResponse(std::string_view body) {
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body);
    } catch (...) {
        return std::nullopt;
    }
    if (!json.is_object()) {
        return std::nullopt;
    }
    TokenResponse out;
    for (const auto& [key, value] : json.items()) {
        out.fields.push_back(key);
    }
    out.nex_token = StringField(json, "nex_token");
    out.access_token = StringField(json, "access_token");
    out.refresh_token = StringField(json, "refresh_token");
    out.expires_in = IntField(json, "expires_in");
    ReadIdentity(json, out.pid, out.username, out.friend_code);
    if (const auto it = json.find("account"); it != json.end()) {
        ReadIdentity(*it, out.pid, out.username, out.friend_code);
    }
    return out;
}

SignInResult SignIn(std::string_view client_id,
                    const std::function<void(const std::string&)>& open_url) {
    SignInResult out;
    if (client_id.empty()) {
        out.error = "No Nextendo client id is set.";
        return out;
    }

    const Pkce pkce = MakePkce();
    const std::string state = RandomToken();

    std::mutex mutex;
    std::condition_variable cv;
    std::optional<Callback> callback;

    httplib::Server server;
    server.Get("/callback", [&](const httplib::Request& request, httplib::Response& response) {
        Callback parsed = ParseCallback(request.params, state);
        response.set_content(parsed.error.empty()
                                 ? "Signed in to Nextendo. You can close this tab and go back to Lichii."
                                 : "Nextendo sign-in failed: " + parsed.error,
                             "text/plain; charset=utf-8");
        {
            std::scoped_lock lock{mutex};
            callback = std::move(parsed);
        }
        cv.notify_all();
    });
    if (!server.bind_to_port("127.0.0.1", CallbackPort)) {
        out.error = fmt::format(
            "Lichii could not listen on 127.0.0.1:{}. Close whatever uses that port and try again.",
            CallbackPort);
        return out;
    }
    std::thread listener{[&server] { void(server.listen_after_bind()); }};

    open_url(BuildAuthorizeUrl(client_id, state, pkce.challenge));
    {
        std::unique_lock lock{mutex};
        cv.wait_for(lock, std::chrono::minutes{5}, [&callback] { return callback.has_value(); });
    }
    server.stop();
    listener.join();

    if (!callback) {
        out.error = "Sign-in timed out.";
        return out;
    }
    if (!callback->error.empty()) {
        out.error = callback->error;
        return out;
    }

    httplib::Client client{std::string{ApiBase}};
    ConfigureClient(client, 15);
    const httplib::Params form{
        {"grant_type", "authorization_code"}, {"code", callback->code},
        {"client_id", std::string{client_id}}, {"redirect_uri", RedirectUri()},
        {"code_verifier", pkce.verifier},
    };
    const auto token =
        ReadReply(client.Post(std::string{TokenPath}, BaseHeaders(client_id), form), "sign-in",
                  out.error);
    if (!token) {
        return out;
    }
    out.token_fields = token->fields;
    if (token->access_token.empty() && token->nex_token.empty()) {
        out.error = "nextendo.network returned no token.";
        return out;
    }

    Account& account = out.account;
    ApplyTokens(account, *token);
    if (account.nex_token.empty() &&
        !FetchGameToken(client, client_id, account, out.game_token_error)) {
        LOG_WARNING(Common, "Signed in to Nextendo without a game token: {}", out.game_token_error);
    }

    if (account.pid == 0 && !account.access_token.empty()) {
        auto headers = BaseHeaders(client_id);
        headers.emplace("Authorization", "Bearer " + account.access_token);
        if (const auto info = client.Get(std::string{UserInfoPath}, headers);
            info && info->status == 200) {
            try {
                const auto json = nlohmann::json::parse(info->body);
                ReadIdentity(json, account.pid, account.username, account.friend_code);
                ReadIdentity(json.value("account", nlohmann::json{}), account.pid,
                             account.username, account.friend_code);
            } catch (...) {
            }
        } else {
            LOG_WARNING(Common, "Nextendo user info request failed (HTTP {})",
                        info ? info->status : 0);
        }
    }

    SaveAccount(account);
    out.ok = true;
    return out;
}

std::optional<Account> EnsureGameToken(std::string_view client_id) {
    std::scoped_lock lock{refresh_mutex};
    u64 generation;
    {
        std::scoped_lock account_lock{account_mutex};
        generation = account_generation;
    }
    auto account = LoadAccount();
    const s64 now = Now();
    if (!account || (account->nex_expires > now + 60 && !account->nex_token.empty()) ||
        (account->access_token.empty() && account->refresh_token.empty())) {
        return account;
    }
    // A game asks for its id token on the IPC thread, so an unreachable server is retried at most
    // every five minutes and with short timeouts.
    if (now - last_renewal_failure < 5 * 60) {
        return account;
    }

    httplib::Client client{std::string{ApiBase}};
    ConfigureClient(client, 5);
    std::string error;
    if (account->access_token.empty() || account->access_expires <= now + 60) {
        if (account->refresh_token.empty()) {
            LOG_WARNING(Common, "The Nextendo session expired; sign in again");
            return account;
        }
        const httplib::Params form{
            {"grant_type", "refresh_token"},
            {"refresh_token", account->refresh_token},
            {"client_id", std::string{client_id}},
        };
        const auto token = ReadReply(
            client.Post(std::string{TokenPath}, BaseHeaders(client_id), form), "token refresh", error);
        if (!token || token->access_token.empty()) {
            last_renewal_failure = now;
            return account;
        }
        ApplyTokens(*account, *token);
        // The server retires the old refresh token a minute after handing out a new one.
        if (!SaveRenewedAccount(*account, generation)) {
            return LoadAccount();
        }
    }
    if (FetchGameToken(client, client_id, *account, error)) {
        if (!SaveRenewedAccount(*account, generation)) {
            return LoadAccount();
        }
    } else {
        last_renewal_failure = now;
        LOG_WARNING(Common, "Could not renew the Nextendo game token: {}", error);
    }
    return account;
}

} // namespace Common::Nextendo
