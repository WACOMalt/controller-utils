#include "archive.h"
#include "compatibility.h"
#include "platform/linux/files.h"
#include "platform/linux/protected_directory.h"
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>

namespace ct::update {
namespace {
void check(int result, const char *operation) {
    if (result < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
void archive_check(int result, struct archive *source) {
    if (result != ARCHIVE_OK)
        throw std::invalid_argument(
            std::string("Invalid update archive: ") +
            (archive_error_string(source) ? archive_error_string(source) : "read failed"));
}
FileDescriptor output_file(const ProtectedDirectory &stage, const std::string &name) {
    stage.verify();
    FileDescriptor parent(fcntl(stage.descriptor(), F_DUPFD_CLOEXEC, 3));
    if (!parent)
        check(-1, "pin update stage");
    const std::filesystem::path path(name);
    for (const auto &part : path.parent_path()) {
        if (mkdirat(parent.get(), part.c_str(), 0755) && errno != EEXIST)
            check(-1, "create update directory");
        check(fsync(parent.get()), "flush update directory");
        FileDescriptor next(
            openat(parent.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next)
            check(-1, "open update directory");
        struct stat info{};
        check(fstat(next.get(), &info), "inspect update directory");
        if (info.st_uid != getuid() || (info.st_mode & 0022))
            throw std::invalid_argument("Unsafe update directory");
        check(fchmod(next.get(), 0755), "set update directory permissions");
        parent = std::move(next);
    }
    FileDescriptor file(openat(parent.get(), path.filename().c_str(),
                               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!file)
        check(-1, "create update file");
    check(fsync(parent.get()), "flush update filename");
    return file;
}
void write_all(int output, const char *bytes, size_t size) {
    while (size) {
        const auto count = write(output, bytes, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::system_error(count < 0 ? errno : EIO, std::generic_category(),
                                    "write update file");
        bytes += count;
        size -= size_t(count);
    }
}
void program(const ProtectedDirectory &stage, const std::string &name) {
    const auto directory = std::filesystem::path(name).parent_path();
    ProtectedDirectory parent(directory.empty() ? stage.path() : stage.path() / directory,
                              getuid());
    auto file = parent.file(std::filesystem::path(name).filename(), MaxContents);
    Elf64_Ehdr header{};
    ssize_t size;
    do {
        size = pread(file.get(), &header, sizeof(header), 0);
    } while (size < 0 && errno == EINTR);
    struct stat info{};
    check(fstat(file.get(), &info), "inspect native update program");
    if (size != sizeof(header) || std::memcmp(header.e_ident, ELFMAG, SELFMAG) ||
        header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_ident[EI_VERSION] != EV_CURRENT || header.e_machine != EM_AARCH64 ||
        header.e_version != EV_CURRENT || header.e_ehsize != sizeof(header) ||
        (header.e_type != ET_DYN && header.e_type != ET_EXEC) || !header.e_phnum ||
        header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phoff < sizeof(header) ||
        header.e_phoff > uint64_t(info.st_size) ||
        uint64_t(header.e_phnum) * sizeof(Elf64_Phdr) > uint64_t(info.st_size) - header.e_phoff)
        throw std::invalid_argument(
            "Update contains an invalid program or a program for the wrong CPU");
    parent.verify_file(std::filesystem::path(name).filename(), file.get());
}
} // namespace
void require_space(const std::filesystem::path &directory, uint64_t bytes) {
    const auto available = std::filesystem::space(directory).available;
    constexpr uint64_t reserve = 16 * 1024 * 1024;
    if (available < bytes || available - bytes < reserve)
        throw std::runtime_error("Not enough free space to install this update");
}
bool allowed_member(const std::string &name) {
    if (name == "controller-utils" || name == "release.json" || name == "LICENSE" ||
        name == "README.md" || name == "third_party/openvr/LICENSE")
        return true;
    for (const auto part : PackageFiles)
        if (name == part)
            return true;
    for (const auto part : DriverFiles)
        if (name == "native-driver/" + std::string(part))
            return true;
    for (const auto &part : helper_files(TrackingBackend::Tracker))
        if (name == "compatibility/" + part)
            return true;
    return std::any_of(Assets.begin(), Assets.end(),
                       [&](const char *asset) { return name == "assets/" + std::string(asset); });
}
void unpack(const std::filesystem::path &path, const std::filesystem::path &destination,
            const Metadata &expected) {
    if (!expected.size || expected.size > MaxArchive)
        throw std::invalid_argument("Invalid update archive size");
    FileDescriptor input(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!input)
        check(-1, "open downloaded update");
    const auto bytes = read_file(input.get(), size_t(expected.size));
    if (bytes.size() != expected.size ||
        sha256_bytes(bytes.data(), bytes.size()) != expected.sha256)
        throw std::invalid_argument("Downloaded update checksum mismatch");
    if (!destination.is_absolute() || destination.lexically_normal() != destination ||
        destination.filename().empty() || destination.string().find('\0') != std::string::npos)
        throw std::invalid_argument("Update extraction requires a new stage directory");
    ProtectedDirectory parent(destination.parent_path(), getuid());
    check(mkdirat(parent.descriptor(), destination.filename().c_str(), 0700),
          "create update stage");
    parent.verify();
    ProtectedDirectory stage(destination, getuid());
    check(fchmod(stage.descriptor(), 0700), "protect update stage");
    std::unique_ptr<struct archive, decltype(&archive_read_free)> source(archive_read_new(),
                                                                         archive_read_free);
    if (!source)
        throw std::bad_alloc();
    archive_check(archive_read_support_filter_gzip(source.get()), source.get());
    archive_check(archive_read_support_format_tar(source.get()), source.get());
    archive_check(archive_read_open_memory(source.get(), bytes.data(), bytes.size()), source.get());
    if (archive_filter_count(source.get()) != 2 ||
        archive_filter_code(source.get(), 0) != ARCHIVE_FILTER_GZIP)
        throw std::invalid_argument("Update archive must contain one gzip layer");
    std::set<std::string> seen;
    uint64_t total = 0;
    for (;;) {
        struct archive_entry *entry = nullptr;
        const auto status = archive_read_next_header(source.get(), &entry);
        if (archive_filter_bytes(source.get(), 0) > int64_t(MaxContents + 1024 * 1024))
            throw std::invalid_argument("Update archive exceeded its contents limit");
        if (status == ARCHIVE_EOF)
            break;
        archive_check(status, source.get());
        const auto name = archive_entry_pathname(entry);
        const auto size = archive_entry_size(entry);
        if (!name || !allowed_member(name) || !archive_entry_size_is_set(entry) || size < 0 ||
            uint64_t(size) > MaxContents || archive_entry_filetype(entry) != AE_IFREG ||
            archive_entry_hardlink(entry) || archive_entry_symlink(entry) ||
            archive_entry_sparse_count(entry) || !seen.insert(name).second)
            throw std::invalid_argument("Unsafe or unexpected file in update archive");
        total += uint64_t(size);
        if (total > MaxContents)
            throw std::invalid_argument("Update archive exceeded its contents limit");
        require_space(stage.path(), uint64_t(size));
        auto output = output_file(stage, name);
        std::array<char, 65536> buffer;
        uint64_t written = 0;
        for (;;) {
            const auto count = archive_read_data(source.get(), buffer.data(), buffer.size());
            if (count < 0)
                archive_check(int(count), source.get());
            if (!count)
                break;
            if (uint64_t(count) > uint64_t(size) - written)
                throw std::invalid_argument("Update archive file exceeded its declared size");
            write_all(output.get(), buffer.data(), size_t(count));
            written += uint64_t(count);
        }
        if (written != uint64_t(size))
            throw std::invalid_argument("Update archive file is truncated");
        check(fchmod(output.get(),
                     (std::string(name) == "controller-utils" ||
                      std::string(name) == "controller-utils-launcher" ||
                      std::string(name) == "install.sh" || std::string(name) == "uninstall.sh" ||
                      std::string(name) == "compatibility/controller-utils-helper" ||
                      std::string(name) == "compatibility/controller-utils-updater")
                         ? 0755
                         : 0644),
              "set update file permissions");
        check(fsync(output.get()), "flush update file");
    }
    archive_check(archive_read_close(source.get()), source.get());
    for (const auto name :
         {"controller-utils", "release.json", "LICENSE", "third_party/openvr/LICENSE",
          "assets/controller.stl", "assets/controller.surface", "assets/controller.buttons",
          "assets/controller-icon.png", "assets/controller-icon-license.txt", "assets/license.txt",
          "assets/README.md", "assets/update-channel.json", "assets/update-public.pem"})
        if (!seen.count(name))
            throw std::invalid_argument(
                "Update archive is incomplete or has the wrong release information");
    for (const auto name : PackageFiles)
        if (!seen.count(name))
            throw std::invalid_argument("Update archive is missing its installation files");
    if (!(release(destination) == expected.release))
        throw std::invalid_argument(
            "Update archive is incomplete or has the wrong release information");
    channel(destination);
    validate_public_key(destination / "assets/update-public.pem");
    if (read_file(destination / "assets/update-public.pem", 16384).empty())
        throw std::invalid_argument("Update signing key is missing");
    check_payload(destination, expected.requirements.get());
    for (const auto &name :
         {std::string("controller-utils"), std::string("controller-utils-launcher"),
          std::string("compatibility/controller-utils-helper"),
          std::string("compatibility/controller-utils-updater"),
          std::string("native-driver/") + DriverFiles[1]})
        program(stage, name);
    program(stage, "compatibility/native-tracker/libnative_tracker.so");
    stage.verify();
    check(fchmod(stage.descriptor(), 0755), "set update stage permissions");
    check(fsync(stage.descriptor()), "flush update stage");
}
} // namespace ct::update
