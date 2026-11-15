#include "compatibility.h"
#include "platform/linux/files.h"
#include "platform/linux/resource.h"
#include <cerrno>
#include <system_error>
#include <tuple>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>

namespace ct::update {
namespace {
bool same(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mode == b.st_mode && a.st_uid == b.st_uid && a.st_gid == b.st_gid &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
std::string fingerprint(const std::filesystem::path &path) {
    if (!path.is_absolute() || path.lexically_normal() != path ||
        path.string().find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid installed component path");
    FileDescriptor file(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (!file)
        throw std::system_error(errno, std::generic_category(), "open installed component");
    struct stat before{}, after{}, named{};
    if (fstat(file.get(), &before))
        throw std::system_error(errno, std::generic_category(), "inspect installed component");
    if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
        uint64_t(before.st_size) > 512 * 1024 * 1024)
        throw std::invalid_argument("Invalid or oversized installed component");
    const auto digest = sha256(file.get());
    if (fstat(file.get(), &after) || stat(path.c_str(), &named))
        throw std::system_error(errno, std::generic_category(), "verify installed component");
    if (!same(before, after) || !same(before, named))
        throw std::runtime_error("Installed component changed while checking compatibility");
    return digest;
}
void add(json_object *object, const std::string &name, const std::string &value) {
    json_object_object_add(object, name.c_str(), json_object_new_string(value.c_str()));
}
} // namespace
InstalledRequirements capture_requirements(const Installation &installation,
                                           TrackingBackend active) {
    struct utsname platform{};
    if (uname(&platform))
        throw std::system_error(errno, std::generic_category(), "read update platform");
    json::Value value(json_object_new_object());
    add(value.get(), "backend", backend_name(active));
    json::Value driver(json_object_new_object()), helper(json_object_new_object()),
        runtime(json_object_new_object());
    for (const auto name : DriverFiles)
        add(driver.get(), name,
            fingerprint(installation.application.parent_path() / "native-driver" / name));
    for (const auto &name : helper_files(active))
        add(helper.get(), name, fingerprint(installation.helpers / name));
    for (size_t index = 0; index < RuntimeFiles.size(); ++index)
        add(runtime.get(), RuntimeFiles[index], fingerprint(installation.runtime[index]));
    json_object_object_add(value.get(), "driver", driver.release());
    json_object_object_add(value.get(), "helper", helper.release());
    json_object_object_add(value.get(), "runtime", runtime.release());
    validate_requirements(value.get());
    return {platform.machine, std::move(value)};
}
void check_requirements(json_object *expected, const InstalledRequirements &installed) {
    validate_requirements(expected);
    validate_requirements(installed.value.get());
    if (installed.machine != "aarch64")
        throw std::runtime_error(
            "This release needs a native compatibility update before it can install");
    for (const auto field : {"backend"})
        if (!json_object_equal(json::field(expected, field),
                               json::field(installed.value.get(), field)))
            throw std::runtime_error(
                "This release needs a native compatibility update before it can install");
    const auto required = json::field(expected, "runtime");
    const auto actual = json::field(installed.value.get(), "runtime");
    for (const char *name : {"XRService"})
        if (!json_object_equal(json::field(required, name), json::field(actual, name)))
            throw std::runtime_error(
                "This release needs a tracking compatibility update before it can install");
}
void check_payload(const std::filesystem::path &stage, json_object *expected) {
    validate_requirements(expected);
    const auto backend = backend_from_name(json::string(json::field(expected, "backend")));
    for (const auto &[field, directory, names] :
         std::vector<std::tuple<const char *, const char *, std::vector<std::string>>>{
             {"driver", "native-driver", {DriverFiles.begin(), DriverFiles.end()}},
             {"helper", "compatibility", helper_files(backend)}})
        for (const auto &name : names)
            if (fingerprint(stage / directory / name) !=
                json::string(json::field(json::field(expected, field), name.c_str())))
                throw std::runtime_error("Release native component checksum mismatch");
}
} // namespace ct::update
