// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <catch2/catch_test_macros.hpp>
#ifdef __APPLE__
#include "common/httplib.h"
#endif
#include "common/net/net.h"
#include "common/scm_rev.h"

// Opt-in: the normal unit suite must work offline. macOS CI runs this with
// OpenSSL's filesystem CA paths disabled to exercise the native trust store.
TEST_CASE("Update requests use a verified HTTPS connection to GitHub", "[.][network]") {
    const auto body = Common::Net::MakeRequest(
        "https://api.github.com",
        std::string{"/repos/"} + Common::g_build_auto_update_repo + "/releases?per_page=1");
    REQUIRE(body.has_value());
    const auto json = nlohmann::json::parse(*body);
    REQUIRE(json.is_array());
}

#ifdef __APPLE__
TEST_CASE("macOS HTTPS preserves explicit CA configuration", "[.][macos_custom_ca]") {
    const char* url = std::getenv("EGLANTINE_TEST_TLS_URL");
    const char* ca_file = std::getenv("EGLANTINE_TEST_TLS_CA_FILE");
    const char* unrelated_ca_file = std::getenv("EGLANTINE_TEST_TLS_UNRELATED_CA_FILE");
    REQUIRE(url);
    REQUIRE(ca_file);
    REQUIRE(unrelated_ca_file);
    const auto check = [&](const char* file, bool should_accept, bool use_store) {
        httplib::Client client(url);
        client.set_connection_timeout(5);
        client.set_read_timeout(5);
        if (use_store) {
            std::ifstream input(file, std::ios::binary);
            REQUIRE(input.good());
            const std::string pem{std::istreambuf_iterator<char>{input},
                                  std::istreambuf_iterator<char>{}};
            client.load_ca_cert_store(pem.data(), pem.size());
        } else {
            client.set_ca_cert_path(file);
        }
        const auto response = client.Get("/");
        REQUIRE(static_cast<bool>(response) == should_accept);
        if (response) {
            REQUIRE(response->status == 200);
        }
    };
    check(ca_file, true, false);
    check(ca_file, true, true);
    check(unrelated_ca_file, false, false);
    check(unrelated_ca_file, false, true);
    httplib::Client missing(url);
    missing.set_ca_cert_path(std::string{ca_file} + ".missing");
    const auto response = missing.Get("/");
    REQUIRE_FALSE(response);
    REQUIRE(response.error() == httplib::Error::SSLLoadingCerts);
}

// Run only via tools/macos/test_https_trust.py on an isolated CI runner.
TEST_CASE("macOS HTTPS enforces native trust for requests and streamed downloads",
          "[.][macos_trust]") {
    const char* url = std::getenv("EGLANTINE_TEST_TLS_URL");
    const char* path = std::getenv("EGLANTINE_TEST_TLS_PATH");
    const char* expected = std::getenv("EGLANTINE_TEST_TLS_ACCEPT");
    REQUIRE(url);
    REQUIRE(path);
    REQUIRE(expected);
    const bool should_accept = std::string_view{expected} == "1";
    const auto body = Common::Net::MakeRequest(url, path);
    REQUIRE(body.has_value() == should_accept);
    if (body) {
        REQUIRE(nlohmann::json::parse(*body).at("ok") == true);
    }

    // The updater downloads through a separate streaming Client, including
    // cross-host redirects. It must enforce the same trust policy.
    httplib::Client client(url);
    client.set_connection_timeout(5);
    client.set_read_timeout(5);
    client.set_follow_location(true);
    std::string downloaded;
    const auto response = client.Get(path, [&](const char* data, size_t size) {
        downloaded.append(data, size);
        return true;
    });
    REQUIRE(static_cast<bool>(response) == should_accept);
    if (response) {
        REQUIRE(response->status == 200);
        REQUIRE(nlohmann::json::parse(downloaded).at("ok") == true);
    } else {
        REQUIRE(downloaded.empty());
    }
}
#endif
