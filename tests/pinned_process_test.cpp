#include "platform/linux/child.h"
#include "platform/linux/files.h"
#include "platform/linux/process.h"
#include "process_image.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <fcntl.h>
#include <sys/mman.h>

namespace {
struct stat mapped_image{};
bool subvolume_device = false;
unsigned mapping_checks = 0;
enum class MappingIdentity { Same, Changed, Unavailable };
MappingIdentity mapping_identity = MappingIdentity::Same;
void subvolume_stat(struct stat &info) {
    if (subvolume_device && info.st_dev == mapped_image.st_dev &&
        info.st_ino == mapped_image.st_ino)
        ++info.st_dev;
}
} // namespace
extern "C" int __real_fstat(int, struct stat *);
extern "C" int __real_fstatat(int, const char *, struct stat *, int);
extern "C" int __wrap_fstat(int fd, struct stat *info) {
    const int result = __real_fstat(fd, info);
    if (!result)
        subvolume_stat(*info);
    return result;
}
extern "C" int __wrap_fstatat(int fd, const char *name, struct stat *info, int flags) {
    if (subvolume_device && std::string(name).rfind("map_files/", 0) == 0) {
        ++mapping_checks;
        if (mapping_identity == MappingIdentity::Unavailable) {
            errno = EPERM;
            return -1;
        }
        *info = mapped_image;
        subvolume_stat(*info);
        if (mapping_identity == MappingIdentity::Changed)
            ++info->st_ino;
        return 0;
    }
    const int result = __real_fstatat(fd, name, info, flags);
    if (!result)
        subvolume_stat(*info);
    return result;
}
namespace {
volatile uint64_t target_value = 7;
int target() {
    std::cout << "{\"address\":" << uintptr_t(&target_value) << ",\"code\":" << uintptr_t(&target)
              << "}\n"
              << std::flush;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "quit")
            return 0;
        std::cout << "{\"value\":" << target_value << "}\n" << std::flush;
    }
    return 0;
}
template <class Function> void rejects(Function function, const std::string &text = "") {
    bool failed = false;
    try {
        function();
    } catch (const std::exception &error) {
        failed = std::string(error.what()).find(text) != std::string::npos;
    }
    assert(failed);
}
ct::json::Value receive(ct::ChildProcess &child) {
    const double deadline = ct::monotonic_time() + 2;
    do {
        auto values = child.poll();
        if (!values.empty()) {
            assert(values.size() == 1);
            return std::move(values[0]);
        }
        ct::sleep_for(.001);
    } while (ct::monotonic_time() < deadline);
    throw std::runtime_error("test child did not reply");
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--target")
        return target();
    std::string stat = "123 (process ) name) S";
    for (unsigned i = 0; i < 18; ++i)
        stat += " 0";
    stat += " 987";
    const auto stamp = ct::process_stamp(stat);
    assert(stamp.state == 'S' && stamp.started == 987);
    rejects([] { ct::process_stamp("invalid"); });
    rejects([] { ct::process_stamp("123 (name) S 0"); });
    auto maps = ct::process_mappings(
        "1000-2000 r-xp 0000 00:01 123 /test image\n2000-3000 rw-p 1000 00:01 123 /test image\n");
    assert(maps.size() == 2 && maps[0].path == "/test image" && maps[0].executable &&
           !maps[0].writable && maps[1].writable);
    assert(maps[0].contains(0x1000, 0x1000) && !maps[0].contains(0x2000, 1));
    assert(!maps[0].contains(0x1001, SIZE_MAX));
    rejects([] { ct::process_mappings("2000-1000 r-xp 0 00:01 123 /test\n"); });
    rejects([] {
        ct::process_mappings(
            "1000-3000 r-xp 0 00:01 123 /test\n2000-4000 rw-p 0 00:01 123 /test\n");
    });
    rejects([] { ct::process_mappings("1000-2000 garbage\n"); });
    {
        const std::string changing =
            "1000-3000 r-xp 0 00:01 123 /test\n2000-4000 rw-p 0 00:01 123 /test\n";
        unsigned reads = 0;
        const auto stable = ct::stable_process_mappings([&] {
            return ++reads < 3 ? changing : std::string("1000-3000 r-xp 0 00:01 123 /test\n");
        });
        assert(reads == 3 && stable.size() == 1);
        reads = 0;
        rejects(
            [&] {
                ct::stable_process_mappings([&] {
                    ++reads;
                    return changing;
                });
            },
            "mappings changed");
        assert(reads == 3);
        reads = 0;
        rejects(
            [&] {
                ct::stable_process_mappings([&] {
                    ++reads;
                    return std::string("invalid\n");
                });
            },
            "invalid native");
        assert(reads == 1);
        reads = 0;
        rejects(
            [&] {
                ct::stable_process_mappings([&]() -> std::string {
                    ++reads;
                    throw std::runtime_error("identity changed");
                });
            },
            "identity changed");
        assert(reads == 1);
    }
    ct::test::ProcessImage image;
    ct::ChildProcess child({image.launch(), "--target"}, std::filesystem::current_path());
    const auto spec = image.spec(child.pid());
    const auto hello = receive(child);
    const auto address =
        uintptr_t(ct::json::unsigned_integer(ct::json::field(hello.get(), "address")));
    const auto code = uintptr_t(ct::json::unsigned_integer(ct::json::field(hello.get(), "code")));
    {
        assert(!::stat(("/proc/" + std::to_string(child.pid()) + "/exe").c_str(), &mapped_image));
        subvolume_device = true;
        {
            ct::PinnedProcess process(child.pid(), spec);
            process.refresh();
            assert(process.field<uint64_t>(address) == 7 && mapping_checks > 1);
        }
        mapping_identity = MappingIdentity::Changed;
        rejects([&] { ct::PinnedProcess process(child.pid(), spec); }, "layout changed");
        mapping_identity = MappingIdentity::Unavailable;
        rejects([&] { ct::PinnedProcess process(child.pid(), spec); }, "layout changed");
        subvolume_device = false;
    }
    {
        auto invalid = spec;
        invalid.owner = getuid() + 1;
        rejects([&] { ct::PinnedProcess process(child.pid(), invalid); }, "identity changed");
        invalid = spec;
        invalid.sha256 = std::string(64, '0');
        rejects([&] { ct::PinnedProcess process(child.pid(), invalid); },
                "unsupported native process binary");
        invalid = spec;
        invalid.image_base = 1;
        rejects([&] { ct::PinnedProcess process(child.pid(), invalid); }, "layout changed");
    }
    ct::PinnedProcess reader(child.pid(), spec);
    assert(reader.alive() && reader.started() > 0 && reader.image_base() > 0);
    assert(reader.field<uint64_t>(address) == 7);
    const auto executable = reader.image();
    assert(ct::sha256_bytes(executable.data(), executable.size()) == spec.sha256);
    rejects([&] { reader.write_data(address, {0}); }, "read-only");
    rejects([&] { reader.read(0, 8); }, "out-of-bounds");
    rejects([&] { reader.read(address, 4097); }, "out-of-bounds");
    rejects([&] { reader.read(UINTPTR_MAX - 3, 8); }, "out-of-bounds");
    ct::PinnedProcess writer(child.pid(), spec, true);
    const uint64_t next = 0x123456789abcdefULL;
    std::vector<uint8_t> bytes(sizeof(next));
    std::memcpy(bytes.data(), &next, sizeof(next));
    writer.write_data(address, bytes);
    assert(reader.field<uint64_t>(address) == next);
    child.send("report\n");
    const auto reply = receive(child);
    assert(ct::json::unsigned_integer(ct::json::field(reply.get(), "value")) == next);
    rejects([&] { writer.write_data(code, bytes); }, "out-of-bounds");
    writer.refresh();
    child.close("quit\n");
    assert(!reader.alive() && !writer.alive());
    rejects([&] { reader.image(); }, "identity changed");
    rejects([&] { reader.read(address, 8); }, "identity changed");
    rejects([&] { writer.write_data(address, bytes); }, "identity changed");
    std::cout << "pinned process: fingerprints, PID identity, mapped bounds and owned-child memory "
                 "passed\n";
}
