#include "helper/package.h"
#include "steamvr/compatibility.h"
#include "platform/linux/files.h"
#include <cassert>
#include <cstring>
#include <elf.h>
#include <iostream>
#include <fcntl.h>

namespace {
std::string digest(const std::string &data) {
    return ct::sha256_bytes(data.data(), data.size());
}
void save(const std::filesystem::path &path, const std::string &data) {
    ct::atomic_write(path, data, 0600);
}
struct Fixture {
    std::filesystem::path root;
    std::string adapter, loader, library;
    Fixture() {
        std::string pattern =
            (std::filesystem::current_path() / "build/native-package-XXXXXX").string();
        const auto directory = mkdtemp(pattern.data());
        assert(directory);
        root = directory;
        auto layout = ct::json::parse(ct::read_file("tests/fixtures/native-adapter.json", 16384));
        const auto hex = ct::read_file("tests/fixtures/native-loader.hex", 4096);
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            loader.push_back(char(std::stoul(hex.substr(i, 2), nullptr, 16)));
        Elf64_Ehdr header{};
        std::memcpy(header.e_ident, ELFMAG, SELFMAG);
        header.e_ident[EI_CLASS] = ELFCLASS64;
        header.e_ident[EI_DATA] = ELFDATA2LSB;
        header.e_ident[EI_VERSION] = EV_CURRENT;
        header.e_type = ET_DYN;
        header.e_machine = EM_AARCH64;
        header.e_version = EV_CURRENT;
        header.e_ehsize = sizeof(header);
        library.assign(reinterpret_cast<const char *>(&header), sizeof(header));
        json_object_object_add(layout.get(), "library_sha256",
                               json_object_new_string(digest(library).c_str()));
        adapter = ct::json::encode(layout.get());
        stage();
    }
    void stage() {
        auto manifest = ct::json::parse("{}");
        for (const auto &[name, data] : std::array<std::pair<const char *, std::string>, 3>{
                 {{"adapter.json", adapter},
                  {"loader.bin", loader},
                  {"libnative_tracker.so", library}}}) {
            save(root / name, data);
            json_object_object_add(manifest.get(), name,
                                   json_object_new_string(digest(data).c_str()));
        }
        save(root / "manifest.json", ct::json::encode(manifest.get()));
    }
    ~Fixture() {
        std::filesystem::remove_all(root);
    }
};
template <class Function> void rejects(Function fn, const std::string &text = "") {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &error) {
        failed = std::string(error.what()).find(text) != std::string::npos;
    }
    assert(failed);
}
} // namespace
int main() {
    using namespace ct;
    Fixture fixture;
    for (const auto fingerprint : steamvr::compatibility::XrServiceHashes) {
        helper::NativePackage package(fingerprint, fixture.root, getuid());
        package.verify({});
    }
    rejects([&] { helper::NativePackage package(std::string(64, '0'), fixture.root, getuid()); },
            "unsupported");
    ProtectedDirectory directory(fixture.root, getuid());
    auto file = directory.file("loader.bin", 1104);
    assert(lseek(file.get(), 2, SEEK_SET) == 2);
    assert(read_file(file.get(), 1104) == fixture.loader);
    assert(lseek(file.get(), 0, SEEK_CUR) == 2);
    rejects([&] { read_file(file.get(), 2); }, "size limit");
    for (const auto *name : {"../loader.bin", ".", "..", "/manifest.json"})
        rejects([&] { directory.file(name, 200000); });
    rejects([&] { ProtectedDirectory other(fixture.root, getuid() + 1); });
    rejects([&] { ProtectedDirectory other(fixture.root / "..", getuid()); });
    std::filesystem::create_symlink(fixture.root / "loader.bin", fixture.root / "linked");
    rejects([&] { directory.file("linked", 1104); });
    std::filesystem::create_hard_link(fixture.root / "loader.bin", fixture.root / "hardlinked");
    rejects([&] { directory.file("loader.bin", 1104); });
    std::filesystem::remove(fixture.root / "hardlinked");
    assert(!mkfifo((fixture.root / "fifo").c_str(), 0600));
    const auto before = monotonic_time();
    rejects([&] { directory.file("fifo", 4096); });
    assert(monotonic_time() - before < .1);
    assert(!chmod((fixture.root / "loader.bin").c_str(), 0660));
    rejects([&] { directory.file("loader.bin", 1104); });
    assert(!chmod((fixture.root / "loader.bin").c_str(), 0600));
    {
        helper::NativePackage package(steamvr::compatibility::XrServiceHashes[0], fixture.root,
                                      getuid());
        package.verify({});
        struct stat info{};
        assert(!stat((fixture.root / "libnative_tracker.so").c_str(), &info));
        MemoryMapping mapping{0x1000,
                              0x2000,
                              0,
                              info.st_dev,
                              uint64_t(info.st_ino),
                              true,
                              false,
                              true,
                              helper::NativePackage::Library};
        package.verify({mapping});
        mapping.path += " (deleted)";
        rejects([&] { package.verify({mapping}); }, "older native adapter");
        mapping.path = helper::NativePackage::Library;
        ++mapping.inode;
        rejects([&] { package.verify({mapping}); }, "older native adapter");
        save(fixture.root / "libnative_tracker.so", fixture.library);
        rejects([&] { package.verify({}); }, "identity changed");
    }
    fixture.stage();
    {
        auto manifest = json::parse(read_file(fixture.root / "manifest.json", 16384));
        json_object_object_add(manifest.get(), "extra.py", json_object_new_string("invalid"));
        save(fixture.root / "manifest.json", json::encode(manifest.get()));
        rejects(
            [&] {
                helper::NativePackage package(steamvr::compatibility::XrServiceHashes[0],
                                              fixture.root, getuid());
            },
            "Unexpected adapter payload");
    }
    fixture.stage();
    save(fixture.root / "loader.bin", std::string(fixture.loader.size(), 'x'));
    rejects(
        [&] {
            helper::NativePackage package(steamvr::compatibility::XrServiceHashes[0], fixture.root,
                                          getuid());
        },
        "checksum mismatch");
    fixture.stage();
    fixture.library[EI_DATA] = ELFDATA2MSB;
    auto layout = json::parse(fixture.adapter);
    json_object_object_add(layout.get(), "library_sha256",
                           json_object_new_string(digest(fixture.library).c_str()));
    fixture.adapter = json::encode(layout.get());
    fixture.stage();
    rejects(
        [&] {
            helper::NativePackage package(steamvr::compatibility::XrServiceHashes[0], fixture.root,
                                          getuid());
        },
        "little-endian ARM64");
    {
        const auto linked =
            fixture.root.parent_path() / (fixture.root.filename().string() + "-link");
        std::filesystem::create_directory_symlink(fixture.root, linked);
        rejects([&] { ProtectedDirectory other(linked, getuid()); });
        std::filesystem::remove(linked);
        const auto moved =
            fixture.root.parent_path() / (fixture.root.filename().string() + "-moved");
        std::filesystem::rename(fixture.root, moved);
        std::filesystem::create_directory(fixture.root);
        rejects([&] { directory.verify(); }, "identity changed");
        std::filesystem::remove(fixture.root);
        std::filesystem::rename(moved, fixture.root);
        assert(!chmod(fixture.root.c_str(), 0770));
        rejects([&] { directory.verify(); }, "Unprotected");
        assert(!chmod(fixture.root.c_str(), 0700));
    }
    std::cout << "native package: protected ancestry, bounded reads, payload checksums, ARM64 "
                 "format and resident identity passed\n";
}
