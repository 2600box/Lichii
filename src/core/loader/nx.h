// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "core/loader/loader.h"

namespace FileSys {
class NACP;
class NxContainer;
} // namespace FileSys

namespace Loader {

/// Loads an NXFormat (.nx) archive. Boots the newest base, update or standalone update snapshot
/// of the program (see NxContainer::FindBootSnapshot); each snapshot is a complete exefs/ and
/// romfs/ tree, so nothing is layered. Catalog reads (title, icon, NACP, program id) do not mount
/// the archive; Load() does. Add-on content snapshots are recognized but not yet mounted, and
/// snapshots stored as native NCAs are not supported.
class AppLoader_NX final : public AppLoader {
public:
    explicit AppLoader_NX(FileSys::VirtualFile file_, u64 program_id = 0);
    ~AppLoader_NX() override;

    static FileType IdentifyType(const FileSys::VirtualFile& nx_file);

    FileType GetFileType() const override {
        return FileType::NX;
    }

    LoadResult Load(Kernel::KProcess& process, Core::System& system) override;

    ResultStatus VerifyIntegrity(std::function<bool(size_t, size_t)> progress_callback) override;

    ResultStatus ReadRomFS(FileSys::VirtualFile& out_file) override;
    bool IsRomFSUpdatable() const override;
    ResultStatus ReadProgramId(u64& out_program_id) override;
    ResultStatus ReadProgramIds(std::vector<u64>& out_program_ids) override;
    ResultStatus ReadIcon(std::vector<u8>& buffer) override;
    ResultStatus ReadTitle(std::string& title) override;
    ResultStatus ReadControlData(FileSys::NACP& control) override;
    ResultStatus ReadNSOModules(Modules& modules) override;

private:
    /// Mounts the archive and opens the boot snapshot's ExeFS.
    ResultStatus OpenProgram();

    std::unique_ptr<FileSys::NxContainer> nx;
    std::optional<std::size_t> boot_snapshot;
    u64 program_id{};
    std::string title;
    u32 icon_media;
    std::unique_ptr<FileSys::NACP> nacp;
    FileSys::VirtualDir snapshot_root;
    std::unique_ptr<AppLoader> secondary_loader;
};

} // namespace Loader
