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

void ReadIdentity(const nlohmann::json& object, u64& pid, std::string& username,
                  std::string& friend_code) {
    if (!object.is_object()) {
        return;
    }
    if (const auto it = object.find("pid"); it != object.end()) {
        pid = ReadPid(*it);
    }
    username = object.value("username", username);
    friend_code = object.value("friend_code", friend_code);
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

void ConfigureClient(httplib::Client& client) {
    client.set_connection_timeout(15);
    client.set_read_timeout(15);
    client.set_write_timeout(15);
    client.set_follow_location(true);
#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
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
        account.nex_token = json.value("nex_token", std::string{});
        account.access_token = json.value("access_token", std::string{});
        account.refresh_token = json.value("refresh_token", std::string{});
        cached_account = std::move(account);
    } catch (const std::exception& e) {
        LOG_WARNING(Common, "Ignoring an unreadable Nextendo account file: {}", e.what());
    }
    return cached_account;
}

void SaveAccount(const Account& account) {
    const nlohmann::json json{
        {"pid", account.pid},
        {"username", account.username},
        {"friend_code", account.friend_code},
        {"nex_token", account.nex_token},
        {"access_token", account.access_token},
        {"refresh_token", account.refresh_token},
    };
    std::scoped_lock lock{account_mutex};
    const auto path = AccountPath();
    void(FS::CreateParentDirs(path));
    if (FS::WriteStringToFile(path, FS::FileType::TextFile, json.dump(2)) == 0) {
        LOG_ERROR(Common, "Could not write the Nextendo account file");
    }
    cached_account = account;
    account_loaded = true;
}

void ClearAccount() {
    std::scoped_lock lock{account_mutex};
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
    out.nex_token = json.value("nex_token", std::string{});
    out.access_token = json.value("access_token", std::string{});
    out.refresh_token = json.value("refresh_token", std::string{});
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
    ConfigureClient(client);
    httplib::Headers headers{{"User-Agent", "Lichii"},
                             {"X-Nextendo-Client-Id", std::string{client_id}}};
    const httplib::Params form{
        {"grant_type", "authorization_code"}, {"code", callback->code},
        {"client_id", std::string{client_id}}, {"redirect_uri", RedirectUri()},
        {"code_verifier", pkce.verifier},
    };
    const auto result = client.Post(std::string{TokenPath}, headers, form);
    if (!result) {
        out.error = "Could not reach nextendo.network.";
        LOG_ERROR(Common, "Nextendo token exchange failed: {}", httplib::to_string(result.error()));
        return out;
    }
    if (result->status != 200) {
        const std::string reason = ServerError(result->body);
        out.error = fmt::format("nextendo.network refused the sign-in (HTTP {}){}{}", result->status,
                                reason.empty() ? "" : ": ", reason);
        LOG_ERROR(Common, "{}", out.error);
        return out;
    }

    const auto token = ParseTokenResponse(result->body);
    if (!token) {
        out.error = "nextendo.network sent an unreadable token response.";
        return out;
    }
    out.token_fields = token->fields;
    LOG_INFO(Common, "Nextendo token response fields: {}", fmt::join(token->fields, ", "));
    if (token->nex_token.empty() && token->access_token.empty()) {
        out.error = "nextendo.network returned no token.";
        return out;
    }

    Account& account = out.account;
    account.nex_token = token->nex_token;
    account.access_token = token->access_token;
    account.refresh_token = token->refresh_token;
    account.pid = token->pid;
    account.username = token->username;
    account.friend_code = token->friend_code;

    if (account.pid == 0 && !account.access_token.empty()) {
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
    if (account.nex_token.empty()) {
        LOG_WARNING(Common, "nextendo.network issued no nex_token to this client id; "
                            "Nextendo game servers will refuse online play");
    }

    SaveAccount(account);
    out.ok = true;
    return out;
}

} // namespace Common::Nextendo
