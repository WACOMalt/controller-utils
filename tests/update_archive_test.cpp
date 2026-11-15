#include "update_fixture.h"
#include "update/archive.h"
#include "platform/linux/files.h"
#include <archive.h>
#include <archive_entry.h>
#include <cassert>
#include <cstring>
#include <elf.h>
#include <iostream>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace u = ct::update;
using namespace update_fixture;
template <class Function> void rejects(Function function, const std::string &reason = "") {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &error) {
        rejected = std::string(error.what()).find(reason) != std::string::npos;
        if (!rejected)
            std::cerr << "Expected rejection containing '" << reason << "', got '" << error.what()
                      << "'\n";
    }
    assert(rejected);
}
} // namespace
int main() {
    const auto root = std::filesystem::current_path() / "build" /
                      ("controller-utils-update-archive-" + std::to_string(getpid()));
    assert(std::filesystem::create_directory(root));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    const auto source = root / "update.tar.gz";
    auto expected = make_archive(source, payload());
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        const auto stage = root / ("stage-" + std::to_string(cycle));
        u::unpack(source, stage, expected);
        assert(u::release(stage) == expected.release);
        assert(ct::read_file(stage / "assets/controller.stl", 100) == "fixture-controller.stl");
        assert(!std::filesystem::exists(stage / "assets/tracking-calibration.json"));
        struct stat info{};
        assert(stat((stage / "controller-utils").c_str(), &info) == 0 &&
               (info.st_mode & 07777) == 0755);
        assert(stat((stage / "assets/controller.stl").c_str(), &info) == 0 &&
               (info.st_mode & 07777) == 0644);
        std::filesystem::remove_all(stage);
    }
    for (const auto bad :
         {"/controller-utils", "../escape", "assets/../../escape", "assets//controller.stl",
          "assets/./controller.stl", "./controller-utils", "tools/unlisted-script.py",
          "build/unlisted-renderer", "assets/tracking-calibration.json", "settings.json",
          "controller-utils-helper", "unlisted"}) {
        assert(!u::allowed_member(bad));
        auto entries = payload();
        entries.push_back({bad, "must not escape", ""});
        expected = make_archive(source, entries);
        rejects([&] { u::unpack(source, root / "unsafe", expected); });
        assert(!std::filesystem::exists(root / "escape"));
        std::filesystem::remove_all(root / "unsafe");
    }
    for (const auto type : {AE_IFDIR, AE_IFIFO, AE_IFLNK}) {
        auto entries = payload();
        entries[4] = {"assets/controller.stl", "", "", mode_t(type)};
        if (type == AE_IFLNK)
            entries[4].link = "../../outside";
        expected = make_archive(source, entries);
        rejects([&] { u::unpack(source, root / "wrong-type", expected); });
        std::filesystem::remove_all(root / "wrong-type");
    }
    {
        auto entries = payload();
        entries[4] = {"assets/controller.stl", "", "controller-utils", AE_IFREG, true};
        expected = make_archive(source, entries);
        rejects([&] { u::unpack(source, root / "hardlink", expected); });
    }
    {
        auto entries = payload();
        entries.push_back(entries[0]);
        expected = make_archive(source, entries);
        rejects([&] { u::unpack(source, root / "duplicate", expected); });
        entries = payload();
        entries.erase(entries.begin() + 2);
        expected = make_archive(source, entries);
        rejects([&] { u::unpack(source, root / "missing-license", expected); });
        entries = payload();
        entries[0].data[18] = 0x3e;
        entries[0].data[19] = 0;
        expected = make_archive(source, entries);
        rejects([&] { u::unpack(source, root / "wrong-cpu", expected); });
        entries = payload();
        entries[0].data.resize(20);
        expected = make_archive(source, entries);
        rejects([&] { u::unpack(source, root / "truncated-program", expected); });
        expected = make_archive(source, payload());
        expected.release.sequence = 3;
        rejects([&] { u::unpack(source, root / "wrong-release", expected); });
        expected = make_archive(source, payload(), false);
        rejects([&] { u::unpack(source, root / "uncompressed", expected); });
        for (const auto name : {"assets/update-channel.json", "assets/update-public.pem"}) {
            auto broken = payload();
            for (auto &entry : broken)
                if (entry.name == name)
                    entry.data = "invalid update configuration";
            expected = make_archive(source, broken);
            rejects([&] {
                u::unpack(source,
                          root / (std::string(name) == "assets/update-channel.json" ? "bad-channel"
                                                                                    : "bad-key"),
                          expected);
            });
        }
    }
    expected = make_archive(source, payload());
    for (const auto name : {"oversized-member.tar.gz", "truncated-member.tar.gz",
                            "nested-gzip.tar.gz", "sparse-member.tar.gz"}) {
        const auto file = std::filesystem::path("tests/fixtures/update-archives") / name;
        u::Metadata fixture{
            {"0.1.1", 2}, 4102444800, std::filesystem::file_size(file), name, ct::sha256(file),
            "fixture",    {}};
        const auto stage = root / name;
        // Older libarchive rejects the sparse header before member validation.
        rejects([&] { u::unpack(file, stage, fixture); },
                std::string(name) == "nested-gzip.tar.gz"        ? "one gzip layer"
                : std::string(name) == "truncated-member.tar.gz" ? "Invalid update archive"
                : std::string(name) == "sparse-member.tar.gz"    ? ""
                                                                 : "Unsafe or unexpected file");
        assert(!std::filesystem::exists(stage / "controller-utils"));
        if (std::string(name) != "truncated-member.tar.gz")
            assert(!std::filesystem::exists(stage / "assets/controller.surface"));
    }
    {
        auto changed = make_archive(source, payload());
        changed.sha256 = std::string(64, '0');
        rejects([&] { u::unpack(source, root / "bad-digest", changed); });
        assert(!std::filesystem::exists(root / "bad-digest"));
        changed = make_archive(source, payload());
        ++changed.size;
        rejects([&] { u::unpack(source, root / "bad-size", changed); });
        assert(!std::filesystem::exists(root / "bad-size"));
        changed.size = u::MaxArchive + 1;
        rejects([&] { u::unpack(source, root / "oversized", changed); });
        assert(!std::filesystem::exists(root / "oversized"));
    }
    {
        const auto existing = root / "existing";
        ct::atomic_write(existing / "sentinel", "keep");
        rejects([&] { u::unpack(source, existing, expected); });
        assert(ct::read_file(existing / "sentinel", 100) == "keep");
        std::filesystem::create_directory_symlink(existing, root / "linked-stage");
        rejects([&] { u::unpack(source, root / "linked-stage", expected); });
        assert(!std::filesystem::exists(existing / "controller-utils"));
        std::filesystem::create_symlink(source, root / "linked-source");
        rejects([&] { u::unpack(root / "linked-source", root / "source-link-stage", expected); });
        std::filesystem::create_directory(root / "writable-parent");
        assert(chmod((root / "writable-parent").c_str(), 0777) == 0);
        rejects([&] { u::unpack(source, root / "writable-parent/stage", expected); });
        assert(!std::filesystem::exists(root / "writable-parent/stage"));
    }
    std::cout << "update archive: 100 stages, checksum-before-write, native payload, ARM64, trust "
                 "and path/type boundaries passed\n";
}
