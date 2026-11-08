// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/loader/loader.h"

namespace FileSys {

/**
 * NXFormat (.nx) archive, read with the vendored nxformat reader (externals/nxformat).
 * Each snapshot is the complete file tree of one version of a program (base, update or
 * standalone update) or of one add-on content package. Opening validates the header and the
 * Catalog only; file trees need Mount(), which validates the Manifest, block index and
 * snapshot identities. Reads verify the CRC-32C of every 16 KiB slice they touch, and
 * decoded Zstandard blocks are kept in a bounded cache.
 */
class NxContainer {
public:
    static constexpr u32 NoMedia = 0xFFFFFFFF;

    enum class SnapshotType : u8 {
        Base = 0,
        Update = 1,
        Addon = 2,
        StandaloneUpdate = 3,
        Unknown = 4,
    };

    struct Application {
        u64 id{};
        /// 0 unknown, 1 Switch 1, 2 Switch 2.
        u8 platform{};
        std::string title;
        std::string publisher;
        u32 icon_media{NoMedia};
        /// (lowercase BCP 47 language, title), each title optional.
        std::vector<std::pair<std::string, std::string>> localized_titles;
        std::vector<std::pair<std::string, u32>> localized_icons;
    };

    struct Program {
        u64 id{};
        std::size_t application{};
        bool primary{};
    };

    struct Snapshot {
        std::size_t application{};
        std::optional<std::size_t> program;
        SnapshotType type{};
        u32 version{};
        std::string display_version;
        std::optional<u64> addon_title_id;
        u32 nacp_media{NoMedia};
        u32 npdm_media{NoMedia};
    };

    static bool IsNxFile(const VirtualFile& file);

    explicit NxContainer(VirtualFile file);
    ~NxContainer();

    NxContainer(const NxContainer&) = delete;
    NxContainer& operator=(const NxContainer&) = delete;

    Loader::ResultStatus GetStatus() const {
        return status;
    }

    const std::vector<Application>& GetApplications() const {
        return applications;
    }

    const std::vector<Program>& GetPrograms() const {
        return programs;
    }

    const std::vector<Snapshot>& GetSnapshots() const {
        return snapshots;
    }

    /// The snapshot that boots `program_id`, or the first primary program when it is 0: the
    /// newest base, update or standalone update by numeric version, an update winning a tie with
    /// a base. A snapshot is a complete view, so it is used on its own; nothing is layered.
    std::optional<std::size_t> FindBootSnapshot(u64 program_id = 0) const;

    /// Media payload (icon, NACP or NPDM) after its SHA-256 check.
    std::optional<std::vector<u8>> ReadMedia(u32 index) const;

    /// Validates the Manifest, block index and snapshot trees once; later calls return the
    /// first result.
    Loader::ResultStatus Mount();

    /// Root of a snapshot's file tree (mounting first if needed), or nullptr on failure. Its
    /// files read through the archive and stay valid after the container is destroyed.
    VirtualDir GetSnapshotDir(std::size_t snapshot);

    /// Checks every media payload, block slice and file SHA-256 (mounting first if needed).
    Loader::ResultStatus VerifyIntegrity(
        const std::function<bool(std::size_t, std::size_t)>& progress_callback);

    struct State;

private:
    std::shared_ptr<State> state;
    Loader::ResultStatus status{Loader::ResultStatus::ErrorNullFile};

    std::vector<Application> applications;
    std::vector<Program> programs;
    std::vector<Snapshot> snapshots;
    std::vector<VirtualDir> snapshot_dirs;
};

} // namespace FileSys
