// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
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
