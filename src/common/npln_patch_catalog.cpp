// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/nextendo.h"
#include "common/npln_patch_catalog.h"
#include "common/npln_patch_manifest.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common::NplnPatchCatalog {
namespace {

// citron-nextendo's verification key for nextendo.network's signed NPLN catalogue (public).
constexpr std::string_view PublicKey = "LAI+Pi+GZtj3p4DpNM5ORBraw1cbZCVSMFgUmAVWNeA=";
constexpr std::size_t MaxDocument = 1024 * 1024;
constexpr auto RefreshInterval = std::chrono::minutes(15);

struct State {
    std::mutex mutex;
    std::once_flag initialized;
    PatchState patches;
    TitleIndexState index;
    std::once_flag index_started;
    std::shared_future<void> index_check;
    std::chrono::steady_clock::time_point index_deadline;
    std::map<std::string, std::chrono::steady_clock::time_point> next_refresh;
};

State& Cache() {
    static State state;
    return state;
}

s64 Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::filesystem::path CacheDir() {
    return FS::GetEdenPath(FS::EdenPath::CacheDir) / "nextendo-npln";
}

std::string TargetName(u64 title_id, std::string_view build_id) {
    return fmt::format("{:016x}-{}", title_id, build_id);
}

std::string ReadDocument(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string body(MaxDocument + 1, '\0');
    input.read(body.data(), static_cast<std::streamsize>(body.size()));
    body.resize(static_cast<std::size_t>(input.gcount()));
    return body;
}

std::vector<std::filesystem::path> Documents(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    for (const auto& file : std::filesystem::directory_iterator(directory, error)) {
        if (file.is_regular_file() && file.path().extension() == ".json") {
            paths.push_back(file.path());
        }
    }
    return paths;
}

// Every kept envelope is verified again on load; nothing unsigned is ever trusted. Earlier
// builds stay available even when a newer catalogue no longer lists them.
void LoadCache() {
    auto& state = Cache();
    for (const auto& path : Documents(CacheDir() / "verified")) {
        try {
            const auto manifest = VerifyManifest(ReadDocument(path), PublicKey, Now(), true);
            std::scoped_lock lock(state.mutex);
            state.patches.Remember(manifest);
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "Nextendo: NPLN patch cache {} rejected: {}",
                        path.filename().string(), error.what());
        }
    }

    // Replays signed discovery in revision order, so partial lists keep known titles.
    std::vector<TitleIndex> indexes;
    for (const auto& path : Documents(CacheDir() / "title-index")) {
        try {
            indexes.push_back(VerifyTitleIndex(ReadDocument(path), PublicKey, Now(), true));
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "Nextendo: NPLN title index cache {} rejected: {}",
                        path.filename().string(), error.what());
        }
    }
    std::sort(indexes.begin(), indexes.end(),
              [](const auto& a, const auto& b) { return a.revision < b.revision; });
    std::scoped_lock lock(state.mutex);
    for (const auto& index : indexes) {
        try {
            state.index.Remember(index);
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "Nextendo: NPLN title index cache rejected: {}", error.what());
        }
    }
}

void SaveDocument(const std::filesystem::path& path, const std::string& envelope) {
    auto temporary = path;
    temporary += ".tmp";
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(envelope.data(), static_cast<std::streamsize>(envelope.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("Cannot write the verified catalog");
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace the verified catalog");
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}

struct Response {
    int status{};
    std::string etag;
    std::string body;
};

// Downloads over HTTPS from nextendo.network, never following a redirect, within 10 seconds and
// MaxDocument bytes.
std::optional<Response> Get(const std::string& path, const std::string& tag) {
    httplib::Client client{std::string{Nextendo::ApiBase}};
    Nextendo::ConfigureClient(client, 5);
    client.set_follow_location(false);
    httplib::Headers headers{{"User-Agent", "Lichii"},
                             {"Accept", "application/json"},
                             {"Cache-Control", "no-cache"}};
    if (!tag.empty()) {
        headers.emplace("If-None-Match", tag);
    }
    Response response;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto result = client.Get(path, headers, [&](const char* data, std::size_t length) {
        if (std::chrono::steady_clock::now() >= deadline ||
            length > MaxDocument - response.body.size()) {
            return false;
        }
        response.body.append(data, length);
        return true;
    });
    if (!result) {
        return std::nullopt;
    }
    response.status = result->status;
    response.etag = result->get_header_value("ETag");
    return response;
}

void DownloadTitleIndex() {
    try {
        auto& state = Cache();
        std::call_once(state.initialized, LoadCache);
        std::string tag;
        {
            std::scoped_lock lock(state.mutex);
            tag = state.index.Tag();
        }
        const auto response = Get("/service-catalog/v1/npln-title-index", tag);
        if (response && response->status == 304 && !tag.empty() && response->etag == tag) {
            LOG_INFO(WebService, "Nextendo: NPLN title index unchanged");
            return;
        }
        if (!response || response->status != 200) {
            throw std::runtime_error("Title index download unavailable");
        }
        const auto index = VerifyTitleIndex(response->body, PublicKey, Now());
        if (response->etag != index.etag) {
            throw std::runtime_error("Title index response tag mismatch");
        }
        std::scoped_lock lock(state.mutex);
        auto next = state.index;
        next.Remember(index);
        SaveDocument(CacheDir() / "title-index" / fmt::format("revision-{}.json", index.revision),
                     response->body);
        state.index = std::move(next);
        LOG_INFO(WebService, "Nextendo: NPLN title index verified, revision {}, {} titles",
                 index.revision, index.titles.size());
    } catch (const std::exception& error) {
        LOG_WARNING(WebService, "Nextendo: NPLN title index check failed, keeping the verified "
                                "index: {}",
                    error.what());
    }
}

void Download(u64 title_id, const std::string& build_id) {
    const auto target = TargetName(title_id, build_id);
    try {
        auto& state = Cache();
        std::string tag;
        {
            std::scoped_lock lock(state.mutex);
            tag = state.patches.Tag(title_id, build_id);
        }
        const auto response = Get(
            fmt::format("/service-catalog/v1/npln-patches/{:016x}/{}", title_id, build_id), tag);
        if (response && response->status == 304 && !tag.empty() && response->etag == tag) {
            LOG_INFO(WebService, "Nextendo: NPLN patch for {} unchanged", target);
            return;
        }
        if (!response || response->status != 200) {
            throw std::runtime_error("Patch download unavailable");
        }
        const auto manifest =
            VerifyBuildManifest(response->body, PublicKey, Now(), title_id, build_id);
        if (response->etag != manifest.etag) {
            throw std::runtime_error("Patch response tag mismatch");
        }
        std::scoped_lock lock(state.mutex);
        if (manifest.revision < state.patches.Revision(title_id, build_id)) {
            throw std::runtime_error("Patch catalog revision rollback rejected");
        }
        try {
            SaveDocument(CacheDir() / "verified" /
                             fmt::format("{}-revision-{}.json", target, manifest.revision),
                         response->body);
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "Nextendo: NPLN patch cache write failed: {}", error.what());
        }
        state.patches.Remember(manifest);
        LOG_INFO(WebService, "Nextendo: NPLN patch {} v{} verified for build {}",
                 manifest.patches[0].id, manifest.patches[0].version, build_id);
    } catch (const std::exception& error) {
        LOG_WARNING(WebService, "Nextendo: NPLN patch refresh for {} failed, keeping verified "
                                "patches: {}",
                    target, error.what());
    }
}

// Refreshes when the last check of this build, here or in an earlier run, is RefreshInterval old.
bool RefreshDue(State& state, const std::string& target) {
    const auto now = std::chrono::steady_clock::now();
    const auto checks = CacheDir() / "checks" / (target + ".txt");
    if (!state.next_refresh.contains(target)) {
        std::ifstream last_check(checks);
        s64 checked_at{};
        const s64 wall_now = Now();
        const s64 interval =
            std::chrono::duration_cast<std::chrono::seconds>(RefreshInterval).count();
        if (last_check >> checked_at && checked_at > 0 && checked_at <= wall_now &&
            wall_now - checked_at < interval) {
            state.next_refresh[target] =
                now + std::chrono::seconds(interval - (wall_now - checked_at));
        }
    }
    if (now < state.next_refresh[target]) {
        return false;
    }
    state.next_refresh[target] = now + RefreshInterval;
    try {
        std::filesystem::create_directories(checks.parent_path());
        std::ofstream last_check(checks, std::ios::trunc);
        last_check << Now() << '\n';
    } catch (const std::exception& error) {
        LOG_WARNING(WebService, "Nextendo: NPLN check time not saved: {}", error.what());
    }
    return true;
}

} // namespace

void PrepareForGame(u64 title_id,
                    const std::function<std::optional<std::string>()>& read_build_id) {
    auto& state = Cache();
    {
        std::scoped_lock lock(state.mutex);
        state.patches.EndGame();
    }
    if (!Nextendo::OnlinePlayActive()) {
        return;
    }
    std::call_once(state.index_started, [&state] {
        state.index_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        state.index_check = std::async(std::launch::async, DownloadTitleIndex).share();
    });
    state.index_check.wait_until(state.index_deadline);
    std::call_once(state.initialized, LoadCache);

    // A title the index stops listing keeps its verified patches until a signed entry withdraws
    // them, so its builds are still refreshed.
    {
        std::scoped_lock lock(state.mutex);
        if (!state.index.HasTitle(title_id) && !state.patches.HasTitle(title_id)) {
            return;
        }
    }
    const auto build_id = read_build_id();
    if (!build_id || build_id->size() != 64 ||
        build_id->find_first_not_of("0123456789ABCDEF") != std::string::npos) {
        LOG_WARNING(WebService, "Nextendo: no main build ID to check the NPLN patch with");
        return;
    }

    bool refresh = false;
    {
        std::scoped_lock lock(state.mutex);
        const auto cached = state.patches.Cached(title_id, *build_id);
        refresh = !(cached && cached->final) && RefreshDue(state, TargetName(title_id, *build_id));
    }
    if (refresh) {
        Download(title_id, *build_id);
    }

    std::scoped_lock lock(state.mutex);
    state.patches.BeginGame(title_id);
    LOG_INFO(WebService, "Nextendo: NPLN patches fixed for this launch, catalog revision {}",
             state.patches.LatestRevision());
}

bool HasEntry(u64 title_id, std::string_view build_id, std::string_view module) {
    if (!Nextendo::OnlinePlayActive()) {
        return false;
    }
    auto& state = Cache();
    std::scoped_lock lock(state.mutex);
    return state.patches.Find(title_id, build_id, module) != nullptr;
}

Lookup Find(u64 title_id, std::string_view build_id, std::string_view module,
            std::span<const u8> image) {
    if (!Nextendo::OnlinePlayActive()) {
        return {};
    }
    auto& state = Cache();
    std::scoped_lock lock(state.mutex);
    const auto patch = state.patches.Find(title_id, build_id, module);
    if (!patch) {
        return {};
    }
    Lookup result{true, patch->id, patch->version, {}};
    if (!patch->enabled) {
        LOG_WARNING(WebService, "Nextendo: NPLN patch {} withdrawn by the signed catalog",
                    patch->id);
        return result;
    }
    if (!CheckGuards(*patch, image)) {
        LOG_ERROR(WebService, "Nextendo: NPLN patch {} rejected, the original instructions do not "
                              "match",
                  patch->id);
        return result;
    }
    result.ips = patch->ips;
    return result;
}

} // namespace Common::NplnPatchCatalog
