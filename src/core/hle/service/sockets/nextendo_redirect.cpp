// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <unordered_map>

#include "common/logging.h"
#include "common/settings.h"
#include "core/hle/service/sockets/nextendo_redirect.h"

namespace Service::Sockets::Nextendo {

namespace {

bool IsDomainOrSubdomain(std::string_view host, std::string_view domain) {
    return host == domain ||
           (host.size() > domain.size() && host.ends_with(domain) &&
            host[host.size() - domain.size() - 1] == '.');
}

std::mutex host_mutex;
std::unordered_map<std::string, std::string> host_for_address;

} // namespace

std::optional<Target> Classify(std::string_view host) {
    // Ryujinx-Nextendo's DnsMitmResolver and Prelude's hosts list put the NAT-check host and
    // Mario Kart 8 Deluxe's NEX server on the second address; everything else is on the first.
    if (host.starts_with("nncs2-") && host.ends_with(".n.n.srv.nintendo.net")) {
        return Target::Nat;
    }
    if (host == "g2b309e01-lp1.s.n.srv.nintendo.net" ||
        host == "g2df33d01-lp1.p.srv.nintendo.net") {
        return Target::Nat;
    }
    for (const std::string_view domain :
         {"nintendo.net", "nintendo.com", "nintendowifi.net", "nintendo.co.jp"}) {
        if (IsDomainOrSubdomain(host, domain)) {
            return Target::Server;
        }
    }
    return std::nullopt;
}

std::optional<std::string> RedirectAddress(std::string_view host) {
    if (!Settings::values.nextendo_enabled.GetValue()) {
        return std::nullopt;
    }
    const auto target = Classify(host);
    if (!target) {
        return std::nullopt;
    }
    std::string address = *target == Target::Nat ? Settings::values.nextendo_nat_ip.GetValue()
                                                 : Settings::values.nextendo_server_ip.GetValue();
    if (address.empty()) {
        return std::nullopt;
    }
    LOG_INFO(Service, "Nextendo: resolving {} to {}", host, address);
    return address;
}

std::string ExpandNsdHost(std::string host) {
    if (const auto pos = host.find('%'); pos != std::string::npos) {
        host.replace(pos, 1, "lp1");
    }
    return host;
}

void RememberHost(const std::string& address, const std::string& host) {
    std::scoped_lock lock{host_mutex};
    host_for_address[address] = host;
}

std::string HostForAddress(const std::string& address) {
    std::scoped_lock lock{host_mutex};
    const auto it = host_for_address.find(address);
    return it == host_for_address.end() ? std::string{} : it->second;
}

} // namespace Service::Sockets::Nextendo
