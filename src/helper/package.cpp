#include "package.h"
#include "platform/linux/files.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <elf.h>
#include <stdexcept>

namespace ct::helper {
NativePackage::NativePackage(const std::string &runtime, std::filesystem::path path, uid_t owner)
    : directory_(std::move(path), owner) {
    auto manifest_file = directory_.file("manifest.json", 16384);
    const auto manifest = json::parse(read_file(manifest_file.get(), 16384));
    if (!json_object_is_type(manifest.get(), json_type_object) ||
        json_object_object_length(manifest.get()) != 3)
        throw std::invalid_argument("Unexpected adapter payload.");
    std::array<std::string, 3> data;
    const char *names[]{"adapter.json", "loader.bin", "libnative_tracker.so"};
    for (size_t i = 0; i < 3; ++i) {
        const auto expected = json::string(json::field(manifest.get(), names[i]));
        if (expected.size() != 64 || !std::all_of(expected.begin(), expected.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
            throw std::invalid_argument("Invalid adapter payload checksum.");
        auto file = directory_.file(names[i], 200000);
        data[i] = read_file(file.get(), 200000);
        directory_.verify_file(names[i], file.get());
        if (data[i].empty() || sha256_bytes(data[i].data(), data[i].size()) != expected)
            throw std::runtime_error("Native adapter checksum mismatch: " + std::string(names[i]));
        if (i == 2)
            library_ = std::move(file);
    }
    directory_.verify_file("manifest.json", manifest_file.get());
    auto adapter = json::parse(data[0]);
    const auto expected_library = json::string(json::field(adapter.get(), "library_sha256"));
    if (expected_library != sha256_bytes(data[2].data(), data[2].size()))
        throw std::runtime_error("Adapter internal checksum mismatch.");
    Elf64_Ehdr header{};
    if (data[2].size() < sizeof(header))
        throw std::invalid_argument("Adapter library is truncated.");
    std::memcpy(&header, data[2].data(), sizeof(header));
    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
        header.e_type != ET_DYN || header.e_machine != EM_AARCH64 ||
        header.e_version != EV_CURRENT || header.e_ehsize != sizeof(header))
        throw std::invalid_argument(
            "Adapter library must be a little-endian ARM64 shared library.");
    layout_ =
        AdapterLayout::from_json(adapter.get(), Bytes(data[1].begin(), data[1].end()), runtime);
    if (fstat(library_.get(), &library_image_))
        throw std::runtime_error("Cannot pin native adapter library.");
    verify({});
}
void NativePackage::verify(const std::vector<MemoryMapping> &mappings) const {
    directory_.verify_file("libnative_tracker.so", library_.get());
    struct stat info{};
    if (fstat(library_.get(), &info) || info.st_size != library_image_.st_size ||
        info.st_mtim.tv_sec != library_image_.st_mtim.tv_sec ||
        info.st_mtim.tv_nsec != library_image_.st_mtim.tv_nsec ||
        info.st_ctim.tv_sec != library_image_.st_ctim.tv_sec ||
        info.st_ctim.tv_nsec != library_image_.st_ctim.tv_nsec)
        throw std::runtime_error("Native adapter library changed.");
    for (const auto &mapping : mappings)
        if (mapping.path.find("libnative_tracker.so") != std::string::npos &&
            (mapping.path != Library || mapping.device != library_image_.st_dev ||
             mapping.inode != uint64_t(library_image_.st_ino)))
            throw std::runtime_error("An older native adapter is resident. Restart SteamVR first.");
}
} // namespace ct::helper
