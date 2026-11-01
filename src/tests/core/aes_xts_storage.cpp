// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/fssystem/fssystem_aes_xts_storage.h"
#include "core/file_sys/fssystem/fssystem_nca_file_system_driver.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {

FileSys::NcaCryptoConfiguration MakeCryptoConfiguration(u8 first, u8 second) {
    FileSys::NcaCryptoConfiguration configuration{};
    configuration.header_encrypted_encryption_keys[0].fill(first);
    configuration.header_encrypted_encryption_keys[1].fill(second);
    configuration.generate_key = [](void* dst, size_t dst_size, const void* src, size_t,
                                    s32 key_type) {
        if (key_type == static_cast<s32>(FileSys::KeyType::NcaHeaderKey1) ||
            key_type == static_cast<s32>(FileSys::KeyType::NcaHeaderKey2)) {
            std::memcpy(dst, src, FileSys::NcaCryptoConfiguration::Aes128KeySize);
        } else {
            std::memset(dst, 0, dst_size);
        }
    };
    return configuration;
}

} // namespace

TEST_CASE("XTS storage rejects duplicate key halves without returning data", "[core][crypto][xts]") {
    using FileSys::AesXtsStorage;
    Core::Crypto::Key128 key{};
    SECTION("Missing key") {}
    SECTION("Repeated nonzero halves") {
        key.fill(0x5A);
    }

    const std::array<u8, AesXtsStorage::IvSize> iv{};
    auto base = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(0x200, 0xA5));
    AesXtsStorage storage(base, key.data(), key.data(), AesXtsStorage::KeySize, iv.data(),
                          iv.size(), 0x200);
    std::array<u8, 0x200> output;
    output.fill(0xCC);
    const auto original = output;
    REQUIRE(storage.Read(output.data(), output.size(), 0) == 0);
    REQUIRE(output == original);
    REQUIRE(storage.Read(output.data(), 0, 0) == 0);
    REQUIRE(base->ReadAllBytes() == std::vector<u8>(0x200, 0xA5));
}

TEST_CASE("NCA header probing handles unavailable XTS keys", "[core][crypto][xts]") {
    auto configuration = MakeCryptoConfiguration(0, 0);
    bool can_decrypt = false;
    SECTION("Missing key") {}
    SECTION("Repeated nonzero halves") {
        configuration = MakeCryptoConfiguration(0x5A, 0x5A);
    }
    SECTION("Distinct halves") {
        configuration = MakeCryptoConfiguration(0x5A, 0xA5);
        can_decrypt = true;
    }
    SECTION("Wrong distinct halves") {
        configuration = MakeCryptoConfiguration(0x17, 0x42);
    }

    FileSys::NcaHeader header{};
    header.magic = FileSys::NcaHeader::Magic3;
    header.sdk_addon_version = 0x000B0000;
    header.key_index = FileSys::NcaCryptoConfiguration::KeyAreaEncryptionKeyIndexZeroKey;
    std::vector<u8> data(sizeof(header));
    std::memcpy(data.data(), &header, sizeof(header));
    auto base = std::make_shared<FileSys::VectorVfsFile>(data);

    FileSys::NcaReader plaintext_reader;
    REQUIRE(R_SUCCEEDED(plaintext_reader.Initialize(base, configuration, {})));
    REQUIRE(plaintext_reader.GetEncryptionType() == FileSys::NcaHeader::EncryptionType::None);
    REQUIRE(plaintext_reader.GetMagic() == FileSys::NcaHeader::Magic3);
    std::vector<u8> decoded_header(sizeof(header));
    plaintext_reader.GetRawData(decoded_header.data(), decoded_header.size());
    REQUIRE(decoded_header == data);

    Core::Crypto::Key256 key{};
    std::fill_n(key.begin(), 16, 0x5A);
    std::fill_n(key.begin() + 16, 16, 0xA5);
    const auto plaintext = data;
    Core::Crypto::AESCipher<Core::Crypto::Key256> cipher(key, Core::Crypto::Mode::XTS);
    cipher.XTSTranscode(data.data(), data.size(), data.data(), 0, FileSys::NcaHeader::XtsBlockSize,
                        Core::Crypto::Op::Encrypt);
    base = std::make_shared<FileSys::VectorVfsFile>(data);
    FileSys::NcaReader encrypted_reader;
    const auto result = encrypted_reader.Initialize(base, configuration, {});
    if (!can_decrypt) {
        REQUIRE(R_FAILED(result));
    } else {
        REQUIRE(R_SUCCEEDED(result));
        REQUIRE(encrypted_reader.GetMagic() == FileSys::NcaHeader::Magic3);
        REQUIRE(encrypted_reader.GetEncryptionType() != FileSys::NcaHeader::EncryptionType::None);
        encrypted_reader.GetRawData(decoded_header.data(), decoded_header.size());
        REQUIRE(decoded_header == plaintext);
    }
}
