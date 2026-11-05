// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <new>
#include <utility>

#include "common/alignment.h"
#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class CommandChunk final {
    class Command {
    public:
        virtual ~Command() = default;

        virtual void Execute(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) const = 0;

        Command* GetNext() const {
            return next;
        }

        void SetNext(Command* next_) {
            next = next_;
        }

    private:
        Command* next = nullptr;
    };

    template <typename T>
    class TypedCommand final : public Command {
    public:
        explicit TypedCommand(T&& command_) : command{std::move(command_)} {}
        ~TypedCommand() override = default;

        TypedCommand(TypedCommand&&) = delete;
        TypedCommand& operator=(TypedCommand&&) = delete;

        void Execute(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) const override {
            command(cmdbuf, upload_cmdbuf);
        }

    private:
        T command;
    };

public:
    CommandChunk() = default;
    ~CommandChunk();

    CommandChunk(const CommandChunk&) = delete;
    CommandChunk& operator=(const CommandChunk&) = delete;

    void ExecuteAll(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf);

    template <typename T>
    bool Record(T& command) {
        using FuncType = TypedCommand<T>;
        static_assert(sizeof(FuncType) < sizeof(data), "Lambda is too large");

        command_offset = Common::AlignUp(command_offset, alignof(FuncType));
        if (command_offset > sizeof(data) - sizeof(FuncType)) {
            return false;
        }
        Command* const current_last = last;
        last = new (data.data() + command_offset) FuncType(std::move(command));

        if (current_last) {
            current_last->SetNext(last);
        } else {
            first = last;
        }
        command_offset += sizeof(FuncType);
        return true;
    }

    void MarkSubmit() {
        submit = true;
    }

    bool Empty() const {
        return command_offset == 0;
    }

    bool HasSubmit() const {
        return submit;
    }

private:
    Command* first = nullptr;
    Command* last = nullptr;

    size_t command_offset = 0;
    bool submit = false;
    alignas(std::max_align_t) std::array<u8, 0x8000> data{};
};

inline CommandChunk::~CommandChunk() {
    while (first != nullptr) {
        auto* command = first;
        first = command->GetNext();
        command->~Command();
    }
}

inline void CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
    while (first != nullptr) {
        auto* command = first;
        command->Execute(cmdbuf, upload_cmdbuf);
        first = command->GetNext();
        command->~Command();
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

} // namespace Vulkan
