// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/nx_container.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

#include <nx_reader.hpp>

#include "common/logging.h"
#include "core/file_sys/nx_zstd.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys {

namespace {

constexpr std::size_t HeaderPageSize = 4096;
constexpr u32 Absent = 0xFFFFFFFF;
constexpr std::array<char, 4> HeaderMagic{'N', 'X', 'A', 'R'};

class VfsSource final : public nx::RandomAccessSource {
public:
    explicit VfsSource(VirtualFile file_) : file{std::move(file_)}, file_size{file->GetSize()} {}

    std::uint64_t size() const noexcept override {
        return file_size;
    }

    bool read_at(std::uint64_t offset, std::uint8_t* destination, std::size_t length) override {
        return offset <= file_size && length <= file_size - offset &&
               file->Read(destination, length, static_cast<std::size_t>(offset)) == length;
    }

private:
    VirtualFile file;
    std::uint64_t file_size;
};

nx::OpenOptions MakeOptions(NxZstd::DecoderPool& decoders) {
    nx::OpenOptions options;
    options.max_cached_block_bytes = 32ULL * 1024 * 1024;
    options.max_cached_blocks = 64;
    options.zstd = &NxZstd::Decode;
    options.zstd_context = &decoders;
    return options;
}

} // namespace

struct NxContainer::State {
    explicit State(VirtualFile file) : source{std::move(file)} {}

    VfsSource source;
    NxZstd::DecoderPool decoders{8};
    nx::Archive archive;
    std::mutex mount_mutex;
    std::optional<Loader::ResultStatus> mount_status;
};

namespace {

using State = NxContainer::State;

/// One file of a mounted snapshot. Reads are split at block boundaries, so each read_file call
/// decodes or verifies one block in place: no read exceeds max_read_bytes and the reader never
/// needs its whole-request staging buffer.
class NxVfsFile final : public VfsFile {
public:
    NxVfsFile(std::shared_ptr<State> state_, nx::FileInfo info_, std::string name_)
        : state{std::move(state_)}, info{info_}, name{std::move(name_)} {}

    std::string GetName() const override {
        return name;
    }

    std::size_t GetSize() const override {
        return static_cast<std::size_t>(info.size);
    }

    bool Resize(std::size_t) override {
        return false;
    }

    VirtualDir GetContainingDirectory() const override {
        return nullptr;
    }

    bool IsWritable() const override {
        return false;
    }

    bool IsReadable() const override {
        return true;
    }

    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        if (offset >= info.size) {
            return 0;
        }
        length = static_cast<std::size_t>(std::min<u64>(length, info.size - offset));
        const u64 chunk_size = u64{1} << info.chunk_log2;
        std::size_t done = 0;
        while (done < length) {
            const u64 position = offset + done;
            const auto piece = static_cast<std::size_t>(
                std::min<u64>(length - done, chunk_size - position % chunk_size));
            const auto status =
                state->archive.read_file(info.id, position, piece, data + done, piece);
            if (!status) {
                LOG_ERROR(Loader, "NX read of {} at {:#x} failed: {}", name, position,
                          status.error.message);
                break;
            }
            done += piece;
        }
        return done;
    }

    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }

    bool Rename(std::string_view) override {
        return false;
    }

private:
    std::shared_ptr<State> state;
    nx::FileInfo info;
    std::string name;
};

/// Depth is bounded by mount(), which rejects trees deeper than 1024 directories.
VirtualDir BuildDirectory(const std::shared_ptr<State>& state, nx::DirectoryHandle handle,
                          std::string name) {
    std::vector<VirtualFile> files;
    std::vector<std::pair<std::string, nx::DirectoryHandle>> children;
    bool files_ok = true;
    const auto status = state->archive.enumerate(handle, [&](const nx::DirectoryEntryView& entry) {
        if (entry.type == nx::EntryType::directory) {
            children.emplace_back(std::string(entry.name), entry.directory);
            return true;
        }
        const auto info = state->archive.file_info(entry.file);
        files_ok = info.ok();
        if (files_ok) {
            files.push_back(std::make_shared<NxVfsFile>(state, *info.value, std::string(entry.name)));
        }
        return files_ok;
    });
    if (!status || !files_ok) {
        return nullptr;
    }
    std::vector<VirtualDir> dirs;
    dirs.reserve(children.size());
    for (auto& [child_name, child] : children) {
        auto dir = BuildDirectory(state, child, std::move(child_name));
        if (dir == nullptr) {
            return nullptr;
        }
        dirs.push_back(std::move(dir));
    }
    return std::make_shared<VectorVfsDirectory>(std::move(files), std::move(dirs), std::move(name));
}

Loader::ResultStatus ToResultStatus(const nx::Error& error) {
    return error.code == nx::ErrorCode::integrity
               ? Loader::ResultStatus::ErrorIntegrityVerificationFailed
               : Loader::ResultStatus::ErrorNXBadFormat;
}

} // namespace

bool NxContainer::IsNxFile(const VirtualFile& file) {
    if (file == nullptr || file->GetSize() < 2 * HeaderPageSize) {
        return false;
    }
    // Either header slot may carry the newest generation; the other may be damaged.
    for (const std::size_t page : {std::size_t{0}, HeaderPageSize}) {
        std::array<char, 4> magic{};
        if (file->ReadObject(&magic, page) == sizeof(magic) && magic == HeaderMagic) {
            return true;
        }
    }
    return false;
}

NxContainer::NxContainer(VirtualFile file) {
    if (file == nullptr) {
        return;
    }
    state = std::make_shared<State>(std::move(file));
    auto opened = nx::Archive::open(state->source, MakeOptions(state->decoders));
    if (!opened) {
        LOG_ERROR(Loader, "NX archive rejected: {}", opened.error.message);
        status = ToResultStatus(opened.error);
        state.reset();
        return;
    }
    auto& archive = state->archive;
    archive = std::move(*opened.value);

    const auto& localized = archive.localized_entries();
    snapshots.resize(archive.snapshots().size());
    for (std::size_t i = 0; i < archive.applications().size(); ++i) {
        const auto& row = archive.applications()[i];
        Application application{
            .id = row.id,
            .platform = row.platform,
            .title = std::string(archive.title(static_cast<u32>(i))),
            .publisher = std::string(archive.publisher(static_cast<u32>(i))),
            .icon_media = row.icon_index,
        };
        for (u32 l = row.first_localized; l < row.first_localized + row.localized_count; ++l) {
            const std::string language(archive.localized_language(l));
            if (localized[l].title_offset != Absent) {
                application.localized_titles.emplace_back(language,
                                                          archive.localized_title(l));
            }
            if (localized[l].icon_index != Absent) {
                application.localized_icons.emplace_back(language, localized[l].icon_index);
            }
        }
        applications.push_back(std::move(application));

        for (u32 s = row.first_snapshot; s < row.first_snapshot + row.snapshot_count; ++s) {
            const auto& snapshot = archive.snapshots()[s];
            snapshots[s] = {
                .application = i,
                .program = snapshot.program_index == Absent
                               ? std::nullopt
                               : std::optional<std::size_t>(snapshot.program_index),
                .type = static_cast<SnapshotType>(std::min<u8>(snapshot.type, 4)),
                .version = snapshot.numeric_version,
                .display_version = std::string(archive.snapshot_display_version(s)),
                .addon_title_id = snapshot.addon_title_id(),
                .nacp_media = snapshot.nacp_index,
                .npdm_media = snapshot.npdm_index,
            };
        }
    }
    for (const auto& program : archive.programs()) {
        programs.push_back({
            .id = program.id,
            .application = program.application_index,
            .primary = program.kind == 0,
        });
    }
    snapshot_dirs.resize(snapshots.size());
    status = Loader::ResultStatus::Success;
}

NxContainer::~NxContainer() = default;

std::optional<std::size_t> NxContainer::FindBootSnapshot(u64 program_id) const {
    std::optional<std::size_t> program;
    for (std::size_t i = 0; i < programs.size() && !program; ++i) {
        if (program_id != 0 ? programs[i].id == program_id : programs[i].primary) {
            program = i;
        }
    }
    if (!program) {
        return std::nullopt;
    }
    std::optional<std::size_t> best;
    const auto rank = [this](std::size_t i) {
        return std::pair{snapshots[i].version, snapshots[i].type != SnapshotType::Base};
    };
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        const auto type = snapshots[i].type;
        if (snapshots[i].program != program ||
            (type != SnapshotType::Base && type != SnapshotType::Update &&
             type != SnapshotType::StandaloneUpdate)) {
            continue;
        }
        if (!best || rank(i) >= rank(*best)) {
            best = i;
        }
    }
    return best;
}

std::optional<std::vector<u8>> NxContainer::ReadMedia(u32 index) const {
    if (state == nullptr || index == NoMedia) {
        return std::nullopt;
    }
    auto media = state->archive.read_media(index);
    if (!media) {
        LOG_ERROR(Loader, "NX media {} rejected: {}", index, media.error.message);
        return std::nullopt;
    }
    return std::move(media.value);
}

Loader::ResultStatus NxContainer::Mount() {
    if (state == nullptr) {
        return status;
    }
    std::scoped_lock lock{state->mount_mutex};
    if (!state->mount_status) {
        const auto mounted = state->archive.mount();
        if (!mounted) {
            LOG_ERROR(Loader, "NX archive mount failed: {}", mounted.error.message);
        }
        state->mount_status = mounted ? Loader::ResultStatus::Success : ToResultStatus(mounted.error);
    }
    return *state->mount_status;
}

VirtualDir NxContainer::GetSnapshotDir(std::size_t snapshot) {
    if (snapshot >= snapshots.size() || Mount() != Loader::ResultStatus::Success) {
        return nullptr;
    }
    if (snapshot_dirs[snapshot] == nullptr) {
        const auto root = state->archive.root_directory(static_cast<u32>(snapshot));
        if (root) {
            snapshot_dirs[snapshot] = BuildDirectory(state, *root.value, "");
        }
    }
    return snapshot_dirs[snapshot];
}

Loader::ResultStatus NxContainer::VerifyIntegrity(
    const std::function<bool(std::size_t, std::size_t)>& progress_callback) {
    if (const auto mounted = Mount(); mounted != Loader::ResultStatus::Success) {
        return mounted;
    }
    if (progress_callback && !progress_callback(0, 1)) {
        return Loader::ResultStatus::ErrorIntegrityVerificationFailed;
    }
    const auto verified = state->archive.verify_all();
    if (!verified) {
        LOG_ERROR(Loader, "NX archive verification failed: {}", verified.error.message);
        return Loader::ResultStatus::ErrorIntegrityVerificationFailed;
    }
    if (progress_callback) {
        progress_callback(1, 1);
    }
    return Loader::ResultStatus::Success;
}

} // namespace FileSys
