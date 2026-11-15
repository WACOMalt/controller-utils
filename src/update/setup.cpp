#include "setup.h"
#include "archive.h"
#include "repair.h"
#include "maintenance.h"
#include "directories.h"
#include "platform/linux/files.h"
#include <iostream>
#include <cerrno>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <linux/fs.h>

namespace ct::update {
namespace {
void check(int value, const char *operation) {
    if (value < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
ProtectedDirectory cache_directory(const UpdateInstallation &installation) {
    ProtectedDirectory parent(installation.cache.parent_path());
    if (mkdirat(parent.descriptor(), installation.cache.filename().c_str(), 0700) &&
        errno != EEXIST)
        check(-1, "create installer cache");
    return ProtectedDirectory(installation.cache);
}
FileDescriptor lock(const ProtectedDirectory &cache) {
    FileDescriptor file(
        openat(cache.descriptor(), "update.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600));
    check(file.get(), "open installer lock");
    cache.verify_file("update.lock", file.get());
    check(flock(file.get(), LOCK_EX | LOCK_NB), "lock package installation");
    return file;
}
void root_only(const UpdateInstallation &installation, const std::function<bool()> &active) {
    if (getuid() != 0 || geteuid() != 0 || installation.user != 1000)
        throw std::runtime_error("Package installation requires root for the Frame user");
    if (active())
        throw std::runtime_error(
            "Close SteamVR and Controller Utils before changing the installation");
}
void policy_destination(const std::filesystem::path &path, const std::string &contents) {
    ProtectedDirectory parent(path.parent_path());
    if (const auto existing = read_record(parent, path.filename()))
        if (*existing != contents)
            throw std::runtime_error("Existing system policy belongs to another installation");
}
void policy(const std::filesystem::path &path, const std::string &contents, mode_t mode,
            bool remove) {
    ProtectedDirectory parent(path.parent_path());
    if (const auto existing = read_record(parent, path.filename())) {
        if (*existing != contents)
            throw std::runtime_error("Existing system policy belongs to another installation");
    }
    if (remove)
        erase_record(parent, path.filename());
    else
        write_record(parent, path.filename(), contents, mode);
}
void trusted_key(const ProtectedDirectory &cache, const std::string &public_key) {
    if (const auto key = read_record(cache, "public.pem", 16384))
        if (*key != public_key)
            throw std::runtime_error("Installed package uses a different signing key");
}
void directory(const std::filesystem::path &path) {
    ProtectedDirectory parent(path.parent_path());
    if (mkdirat(parent.descriptor(), path.filename().c_str(), 0755) && errno != EEXIST)
        check(-1, "create maintenance directory");
    ProtectedDirectory created(path);
}
std::filesystem::path wanted(const SetupPolicies &paths) {
    return paths.service.parent_path() / "multi-user.target.wants";
}
bool maintenance_link(const SetupPolicies &paths) {
    ProtectedDirectory parent(wanted(paths));
    const auto name = paths.service.filename();
    struct stat info{};
    if (fstatat(parent.descriptor(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW)) {
        if (errno == ENOENT)
            return false;
        check(-1, "inspect maintenance registration");
    }
    char target[4096];
    const auto size = readlinkat(parent.descriptor(), name.c_str(), target, sizeof(target));
    if (!S_ISLNK(info.st_mode) || info.st_uid != 0 || size <= 0 ||
        std::string(target, size_t(size)) != "../" + name.string())
        throw std::runtime_error("Existing maintenance registration requires review");
    return true;
}
void prepare_maintenance(const SetupPolicies &paths) {
    directory(paths.keep.parent_path());
    directory(wanted(paths));
    policy_destination(paths.service, MaintenanceService);
    policy_destination(paths.keep, MaintenanceKeep);
    (void)maintenance_link(paths);
}
void install_maintenance(const SetupPolicies &paths) {
    policy(paths.keep, MaintenanceKeep, 0644, false);
    policy(paths.service, MaintenanceService, 0644, false);
    ProtectedDirectory parent(wanted(paths));
    const auto name = paths.service.filename().string();
    if (!maintenance_link(paths)) {
        check(symlinkat(("../" + name).c_str(), parent.descriptor(), name.c_str()),
              "enable maintenance service");
        check(fsync(parent.descriptor()), "flush maintenance registration");
    }
}
void remove_maintenance(const SetupPolicies &paths) {
    if (maintenance_link(paths)) {
        ProtectedDirectory parent(wanted(paths));
        check(unlinkat(parent.descriptor(), paths.service.filename().c_str(), 0),
              "remove maintenance registration");
        check(fsync(parent.descriptor()), "flush maintenance registration");
    }
    policy(paths.service, MaintenanceService, 0644, true);
    policy(paths.keep, MaintenanceKeep, 0644, true);
}
} // namespace
Metadata stage_package(const std::filesystem::path &feed, const std::filesystem::path &destination,
                       const std::string &public_key) {
    auto metadata = verify_metadata_key(feed / "latest.json", feed / "latest.sig", public_key);
    unpack(feed / metadata.artifact, destination, metadata);
    if (read_file(destination / "assets/update-public.pem", 16384) != public_key ||
        channel(destination) != GitHubChannel)
        throw std::runtime_error("Package cannot change its signing key or GitHub repository");
    return metadata;
}
void setup_install(const UpdateInstallation &installation, const SetupPolicies &policies,
                   const std::filesystem::path &feed, const std::string &public_key,
                   const std::function<bool()> &active) {
    root_only(installation, active);
    auto cache = cache_directory(installation);
    const auto lease = lock(cache);
    trusted_key(cache, public_key);
    remove_directory(cache, "setup-stage");
    struct Cleanup {
        const ProtectedDirectory &cache;
        ~Cleanup() {
            try {
                remove_directory(cache, "setup-stage");
            } catch (const std::exception &error) {
                std::cerr << "Package staging cleanup: " << error.what() << '\n';
            }
        }
    } cleanup{cache};
    const auto metadata = stage_package(feed, cache.path() / "setup-stage", public_key);
    const auto history = read_history(cache);
    if (metadata.release.sequence < history.highest_sequence)
        throw std::runtime_error("An older package cannot replace the installed release");
    auto components = installation.components;
    components.application = cache.path() / "setup-stage/app";
    components.helpers = cache.path() / "setup-stage/compatibility";
    check_requirements(
        metadata.requirements.get(),
        capture_requirements(components, backend_from_name(json::string(json::field(
                                             metadata.requirements.get(), "backend")))));
    ProtectedDirectory stage(cache.path() / "setup-stage");
    const auto sudoers =
        read_file(stage.path() / "packaging/controller-utils-updater.sudoers", 16384);
    const auto udev = read_file(stage.path() / "packaging/70-controller-utils.rules", 16384);
    ProtectedDirectory parent(installation.components.helpers.parent_path());
    const auto native = installation.components.helpers.filename().string();
    auto receipt = metadata.release.to_json();
    json_object_object_add(receipt.get(), "sha256",
                           json_object_new_string(metadata.sha256.c_str()));
    if (directory_identity(parent, native)) {
        const auto receipt = read_record(cache, "setup.json");
        if (!receipt ||
            json::string(json::field(json::parse(*receipt).get(), "sha256")) != metadata.sha256)
            throw std::runtime_error(
                "Controller Utils is already installed. Use its updates or uninstall it first");
        for (const auto &name : helper_files(backend_from_name(
                 json::string(json::field(metadata.requirements.get(), "backend"))))) {
            const auto subdirectory = std::filesystem::path(name).parent_path();
            ProtectedDirectory directory(subdirectory.empty()
                                             ? installation.components.helpers
                                             : installation.components.helpers / subdirectory);
            const auto file = directory.file(std::filesystem::path(name).filename(), MaxArchive);
            if (sha256(file.get()) !=
                json::string(
                    json::field(json::field(metadata.requirements.get(), "helper"), name.c_str())))
                throw std::runtime_error(
                    "Installed native package changed. Preserve it for review");
        }
    }
    policy_destination(policies.sudoers, sudoers);
    policy_destination(policies.udev, udev);
    prepare_maintenance(policies);
    {
        ProtectedDirectory native_directory(directory_identity(parent, native)
                                                ? installation.components.helpers
                                                : stage.path() / "compatibility");
        record_repair_evidence(native_directory, read_file(feed / "latest.json", MaxMetadata),
                               read_file(feed / "latest.sig", 64), public_key, metadata.release);
    }
    write_record(cache, "public.pem", public_key, 0644);
    write_record(cache, "sudoers", sudoers);
    write_record(cache, "udev.rules", udev);
    write_record(cache, "setup.json", json::encode(receipt.get()));
    policy(policies.sudoers, sudoers, 0440, false);
    policy(policies.udev, udev, 0644, false);
    if (!directory_identity(parent, native))
        move_directory(stage, "compatibility", parent, native, RENAME_NOREPLACE);
    install_maintenance(policies);
    auto installed_history = history;
    installed_history.highest_sequence = metadata.release.sequence;
    save_history(cache, installed_history);
}
void setup_remove(const UpdateInstallation &installation, const SetupPolicies &policies,
                  const std::string &public_key, const std::function<bool()> &active) {
    root_only(installation, active);
    ProtectedDirectory cache(installation.cache);
    const auto lease = lock(cache);
    trusted_key(cache, public_key);
    const auto sudoers = read_record(cache, "sudoers", 16384);
    const auto udev = read_record(cache, "udev.rules", 16384);
    if (!sudoers)
        throw std::runtime_error("Installed package ownership could not be verified");
    policy_destination(policies.sudoers, *sudoers);
    if (udev)
        policy_destination(policies.udev, *udev);
    policy_destination(policies.service, MaintenanceService);
    policy_destination(policies.keep, MaintenanceKeep);
    (void)maintenance_link(policies);
    remove_maintenance(policies);
    policy(policies.sudoers, *sudoers, 0440, true);
    if (udev)
        policy(policies.udev, *udev, 0644, true);
    ProtectedDirectory native_parent(installation.components.helpers.parent_path());
    remove_directory(native_parent, installation.components.helpers.filename());
    ProtectedDirectory cache_parent(installation.cache.parent_path());
    remove_directory(cache_parent, installation.cache.filename());
}
} // namespace ct::update
