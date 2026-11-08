// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/loader/nx.h"

#include <algorithm>

#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nx_container.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace Loader {

using FileSys::NxContainer;

AppLoader_NX::AppLoader_NX(FileSys::VirtualFile file_, u64 program_id_)
    : AppLoader(std::move(file_)), nx(std::make_unique<NxContainer>(file)),
      icon_media{NxContainer::NoMedia} {
    if (nx->GetStatus() != ResultStatus::Success) {
        return;
    }
    boot_snapshot = nx->FindBootSnapshot(program_id_);
    if (!boot_snapshot) {
        LOG_ERROR(Loader, "NX archive has no base or update snapshot to boot");
        return;
    }
    const auto& snapshots = nx->GetSnapshots();
    const auto& boot = snapshots[*boot_snapshot];
    const auto& application = nx->GetApplications()[boot.application];
    program_id = nx->GetPrograms()[*boot.program].id;

    title = application.title;
    icon_media = application.icon_media;
    for (const auto& [language, icon] : application.localized_icons) {
        if (icon_media == NxContainer::NoMedia || language == "en-us") {
            icon_media = icon;
        }
    }

    // The boot snapshot's own NACP, else the newest one recorded for the same program.
    std::optional<std::size_t> nacp_snapshot;
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        if (snapshots[i].program != boot.program || snapshots[i].nacp_media == NxContainer::NoMedia) {
            continue;
        }
        if (i == *boot_snapshot) {
            nacp_snapshot = i;
            break;
        }
        if (!nacp_snapshot || snapshots[i].version >= snapshots[*nacp_snapshot].version) {
            nacp_snapshot = i;
        }
    }
    if (nacp_snapshot) {
        auto bytes = nx->ReadMedia(snapshots[*nacp_snapshot].nacp_media);
        if (bytes && bytes->size() >= sizeof(FileSys::RawNACP)) {
            nacp = std::make_unique<FileSys::NACP>(
                std::make_shared<FileSys::VectorVfsFile>(std::move(*bytes), "control.nacp"));
        }
    }

    const auto addons = std::count_if(snapshots.begin(), snapshots.end(), [](const auto& s) {
        return s.type == NxContainer::SnapshotType::Addon;
    });
    if (addons > 0) {
        LOG_WARNING(Loader, "NX archive has {} add-on content snapshot(s); AoC mounting is not "
                            "supported yet",
                    addons);
    }
}

AppLoader_NX::~AppLoader_NX() = default;

FileType AppLoader_NX::IdentifyType(const FileSys::VirtualFile& nx_file) {
    return NxContainer::IsNxFile(nx_file) ? FileType::NX : FileType::Error;
}

ResultStatus AppLoader_NX::OpenProgram() {
    if (nx->GetStatus() != ResultStatus::Success) {
        return nx->GetStatus();
    }
    if (!boot_snapshot) {
        return ResultStatus::ErrorNXNoProgram;
    }
    if (secondary_loader != nullptr) {
        return ResultStatus::Success;
    }
    if (const auto mounted = nx->Mount(); mounted != ResultStatus::Success) {
        return mounted;
    }
    snapshot_root = nx->GetSnapshotDir(*boot_snapshot);
    const auto exefs = snapshot_root != nullptr ? snapshot_root->GetSubdirectory("exefs") : nullptr;
    if (exefs == nullptr || exefs->GetFile("main") == nullptr ||
        exefs->GetFile("main.npdm") == nullptr) {
        LOG_ERROR(Loader, "NX snapshot {} has no exefs/ tree with main and main.npdm",
                  *boot_snapshot);
        return ResultStatus::ErrorNXNoProgram;
    }
    secondary_loader = std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs, true);
    return ResultStatus::Success;
}

AppLoader::LoadResult AppLoader_NX::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }
    if (const auto opened = OpenProgram(); opened != ResultStatus::Success) {
        return {opened, {}};
    }
    const auto result = secondary_loader->Load(process, system);
    if (result.first != ResultStatus::Success) {
        return result;
    }
    system.GetFileSystemController().RegisterProcess(
        process.GetProcessId(), program_id,
        std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                system.GetFileSystemController()));
    is_loaded = true;
    return result;
}

ResultStatus AppLoader_NX::VerifyIntegrity(std::function<bool(size_t, size_t)> progress_callback) {
    if (nx->GetStatus() != ResultStatus::Success) {
        return nx->GetStatus();
    }
    return nx->VerifyIntegrity(progress_callback);
}

ResultStatus AppLoader_NX::ReadRomFS(FileSys::VirtualFile& out_file) {
    if (const auto opened = OpenProgram(); opened != ResultStatus::Success) {
        return opened;
    }
    const auto romfs = snapshot_root->GetSubdirectory("romfs");
    out_file = romfs != nullptr ? FileSys::CreateRomFS(romfs) : nullptr;
    return out_file != nullptr ? ResultStatus::Success : ResultStatus::ErrorNoRomFS;
}

bool AppLoader_NX::IsRomFSUpdatable() const {
    // The boot snapshot is already the complete updated tree.
    return false;
}

ResultStatus AppLoader_NX::ReadProgramId(u64& out_program_id) {
    if (!boot_snapshot) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_id = program_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadProgramIds(std::vector<u64>& out_program_ids) {
    if (!boot_snapshot) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_ids.push_back(program_id);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadIcon(std::vector<u8>& buffer) {
    auto icon = nx->ReadMedia(icon_media);
    if (!icon) {
        return ResultStatus::ErrorNoIcon;
    }
    buffer = std::move(*icon);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadTitle(std::string& out_title) {
    out_title = !title.empty() || nacp == nullptr ? title : nacp->GetApplicationName();
    return out_title.empty() ? ResultStatus::ErrorNoControl : ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadControlData(FileSys::NACP& control) {
    if (nacp == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    control = *nacp;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NX::ReadNSOModules(Modules& modules) {
    if (const auto opened = OpenProgram(); opened != ResultStatus::Success) {
        return opened;
    }
    return secondary_loader->ReadNSOModules(modules);
}

} // namespace Loader
