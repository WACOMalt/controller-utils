#include "installer.h"
#include "archive.h"
#include "directories.h"
#include "repair.h"
#include "app/settings.h"
#include "platform/linux/files.h"
#include <cerrno>
#include <grp.h>
#include <iostream>
#include <set>
#include <system_error>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/wait.h>

namespace ct::update {
namespace {
void check(int result, const char *operation) {
    if (result < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
struct NativeJournal {
    bool healthy;
    Release release;
    DirectoryIdentity previous, next;
    std::string application;
    json::Value to_json() const {
        json::Value value(json_object_new_object());
        json_object_object_add(value.get(), "schema", json_object_new_int(1));
        json_object_object_add(value.get(), "healthy", json_object_new_boolean(healthy));
        json_object_object_add(value.get(), "release", release.to_json().release());
        json_object_object_add(value.get(), "previous", previous.to_json().release());
        json_object_object_add(value.get(), "next", next.to_json().release());
        json_object_object_add(value.get(), "application",
                               json_object_new_string(application.c_str()));
        return value;
    }
    static NativeJournal from_json(json_object *value) {
        if (!value || json_object_get_type(value) != json_type_object ||
            json_object_object_length(value) != 6 ||
            json::integer(json::field(value, "schema")) != 1)
            throw std::runtime_error("Invalid native update journal");
        NativeJournal result{json::boolean(json::field(value, "healthy")),
                             Release::from_json(json::field(value, "release")),
                             DirectoryIdentity::from_json(json::field(value, "previous")),
                             DirectoryIdentity::from_json(json::field(value, "next")),
                             json::string(json::field(value, "application"))};
        if (result.previous == result.next || result.previous.device != result.next.device ||
            result.application.size() != 64 ||
            result.application.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Invalid native update journal");
        return result;
    }
};
std::optional<NativeJournal> journal(const ProtectedDirectory &cache) {
    const auto bytes = read_record(cache, "transaction.json");
    if (!bytes)
        return std::nullopt;
    return NativeJournal::from_json(json::parse(*bytes).get());
}
void save(const ProtectedDirectory &cache, const NativeJournal &record) {
    write_record(cache, "transaction.json", json::encode(record.to_json().get()));
}
Release verified_native_release(const ProtectedDirectory &native, const std::string &key) {
    const auto metadata = verify_native_evidence(native, key);
    const auto names = helper_files(
        backend_from_name(json::string(json::field(metadata.requirements.get(), "backend"))));
    std::set<std::filesystem::path> allowed{".repair-evidence", "native-tracker"};
    for (const auto &name : names) {
        const std::filesystem::path relative(name);
        allowed.insert(relative);
        ProtectedDirectory directory(relative.parent_path().empty()
                                         ? native.path()
                                         : native.path() / relative.parent_path());
        const auto file = directory.file(relative.filename().string(), MaxContents);
        if (sha256(file.get()) !=
            json::string(
                json::field(json::field(metadata.requirements.get(), "helper"), name.c_str())))
            throw std::runtime_error("Confirmed native component differs from signed evidence");
        directory.verify_file(relative.filename().string(), file.get());
    }
    for (const auto &entry : std::filesystem::recursive_directory_iterator(native.path()))
        if (entry.is_symlink() || !allowed.count(entry.path().lexically_relative(native.path())))
            throw std::runtime_error("Preserve the changed native backup contents for review");
    native.verify();
    return metadata.release;
}
void retire_confirmed(const UpdateInstallation &installation, const ProtectedDirectory &cache,
                      const ProtectedDirectory &parent) {
    const auto record = journal(cache);
    if (!record || !record->healthy)
        return;
    const auto key = read_record(cache, "public.pem", 16384);
    if (!key)
        throw std::runtime_error("Native update signing key is unavailable");
    if (!(verified_native_release(ProtectedDirectory(installation.components.helpers), *key) ==
          record->release))
        throw std::runtime_error("Confirmed native release differs from signed evidence");
    const auto backup = directory_identity(cache, "previous-native");
    if (backup) {
        const auto previous =
            verified_native_release(ProtectedDirectory(cache.path() / "previous-native"), *key);
        if (previous.sequence >= record->release.sequence)
            throw std::runtime_error("Preserve the changed native backup release for review");
        remove_directory(cache, "previous-native", *backup);
    }
    parent.verify();
    erase_record(cache, "transaction.json");
}
int application_transaction(const UpdateInstallation &installation, int descriptor, bool activate,
                            bool force, uint64_t recovering = 0) {
    const auto child = fork();
    check(child, "fork application update");
    if (!child) {
        try {
            check(setgroups(0, nullptr), "drop update supplementary groups");
            check(setgid(installation.group), "drop update group");
            check(setuid(installation.user), "drop update privileges");
            ApplicationLock lock(installation.data / "app.lock", descriptor);
            MaintenanceLock maintenance(installation.data);
            Transaction transaction(
                installation.components.application, installation.data, lock, maintenance, [&] {
                    return capture_requirements(installation.components,
                                                installed_backend(installation.components.helpers));
                });
            if (activate)
                _exit(transaction.activate() ? 0 : 2);
            if (force) {
                const auto bytes = read_record(maintenance.cache(), "transaction.json");
                if (!bytes ||
                    Release::from_json(json::field(json::parse(*bytes).get(), "next")).sequence !=
                        recovering)
                    _exit(0);
            }
            transaction.recover(force);
            _exit(0);
        } catch (const std::exception &error) {
            std::cerr << "Controller Utils application update: " << error.what() << '\n';
            _exit(1);
        }
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            check(-1, "wait for application update");
    if (!WIFEXITED(status))
        throw std::runtime_error("Application update stopped unexpectedly");
    return WEXITSTATUS(status);
}
void recover(const UpdateInstallation &installation, const ProtectedDirectory &cache,
             const ProtectedDirectory &parent, int application_lock) {
    const auto record = journal(cache);
    if (!record || record->healthy)
        return;
    const auto name = installation.components.helpers.filename().string();
    const auto live = directory_identity(parent, name);
    const auto previous = directory_identity(cache, "previous-native");
    if ((!live || live == record->next) && previous == record->previous)
        move_directory(cache, "previous-native", parent, name,
                       live ? RENAME_EXCHANGE : RENAME_NOREPLACE);
    else if (live != record->previous)
        throw std::runtime_error("Native update recovery identities changed");
    if (application_transaction(installation, application_lock, false, true,
                                record->release.sequence) != 0)
        throw std::runtime_error("Application update recovery failed");
    remove_directory(cache, "previous-native", record->next);
    remove_directory(cache, "candidate");
    erase_record(cache, "transaction.json");
}
void snapshot(const ProtectedDirectory &source, const std::string &name,
              const ProtectedDirectory &destination, size_t limit, const std::string &target) {
    const auto input = source.file(name, limit);
    const auto bytes = read_file(input.get(), limit);
    source.verify_file(name, input.get());
    require_space(destination.path(), bytes.size());
    write_record(destination, target, bytes);
}
} // namespace
bool runtime_running() {
    for (const auto &entry : std::filesystem::directory_iterator("/proc")) {
        const auto name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::error_code error;
        const auto executable = std::filesystem::read_symlink(entry.path() / "exe", error);
        if (!error &&
            (executable.filename() == "vrserver" || executable.filename() == "XRService" ||
             executable.filename() == "vrcompositor" ||
             executable.filename() == "controller-utils-helper"))
            return true;
    }
    return false;
}
void install_update(const UpdateInstallation &installation, bool confirmation,
                    const std::function<bool()> &active) {
    if (getuid() != 0 || geteuid() != 0 || !installation.user || !installation.group)
        throw std::runtime_error(
            "Native updates require a root installer and an unprivileged application owner");
    ProtectedDirectory cache(installation.cache);
    FileDescriptor lock(
        openat(cache.descriptor(), "update.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    check(lock.get(), "open native update lock");
    cache.verify_file("update.lock", lock.get());
    check(flock(lock.get(), LOCK_EX | LOCK_NB), "lock native updater");
    ProtectedDirectory parent(installation.components.helpers.parent_path());
    ProtectedDirectory data(installation.data, installation.user);
    retire_confirmed(installation, cache, parent);
    if (confirmation) {
        auto record = journal(cache);
        if (!record)
            return;
        const auto application = installation.components.application;
        ProtectedDirectory app(application, installation.user);
        const auto executable = app.file("controller-utils", MaxContents);
        if (directory_identity(parent, installation.components.helpers.filename()) !=
                record->next ||
            !(release(application) == record->release) ||
            sha256(executable.get()) != record->application)
            throw std::runtime_error("The installed application does not match the signed release");
        ProtectedDirectory updates(installation.data / "updates", installation.user);
        const auto state = read_record(updates, "transaction.json");
        if (!state || json::string(json::field(json::parse(*state).get(), "phase")) != "healthy")
            throw std::runtime_error("The application has not confirmed startup");
        app.verify_file("controller-utils", executable.get());
        record->healthy = true;
        save(cache, *record);
        retire_confirmed(installation, cache, parent);
        return;
    }
    if (active()) {
        std::cout << "SteamVR is running. Update deferred until its next startup.\n";
        return;
    }
    FileDescriptor lease(
        openat(data.descriptor(), "app.lock", O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    check(lease.get(), "open application update lock");
    data.verify_file("app.lock", lease.get());
    check(flock(lease.get(), LOCK_EX | LOCK_NB), "lock application before native update");
    recover(installation, cache, parent, lease.get());
    remove_directory(cache, "candidate");
    erase_record(cache, "archive.tar.gz");
    struct Cleanup {
        const ProtectedDirectory &cache;
        ~Cleanup() {
            try {
                remove_directory(cache, "candidate");
                erase_record(cache, "archive.tar.gz");
            } catch (const std::exception &error) {
                std::cerr << "Controller Utils update cleanup: " << error.what() << '\n';
            }
        }
    } cleanup{cache};
    if (!std::filesystem::exists(installation.data / "updates/pending"))
        return;
    ProtectedDirectory pending(installation.data / "updates/pending", installation.user);
    snapshot(pending, "latest.json", cache, MaxMetadata, "latest.json");
    snapshot(pending, "latest.sig", cache, 64, "latest.sig");
    auto history = read_history(cache);
    const auto metadata = verify_metadata(cache.path() / "latest.json", cache.path() / "latest.sig",
                                          cache.path() / "public.pem", history.highest_sequence);
    const auto current = release(installation.components.application);
    if (metadata.release.sequence <= current.sequence)
        return;
    const auto settings = read_record(data, "settings.json", 131072);
    const auto options = settings ? Settings::from_json(json::parse(*settings).get()) : Settings{};
    ProtectedDirectory user_cache(installation.data / "updates", installation.user);
    const auto user_history = read_history(user_cache);
    if ((!options.automatic_updates || history.failed_sequence == metadata.release.sequence ||
         user_history.failed_sequence == metadata.release.sequence) &&
        user_history.manual_sequence != metadata.release.sequence)
        return;
    check_requirements(metadata.requirements.get(),
                       capture_requirements(installation.components,
                                            installed_backend(installation.components.helpers)));
    history.highest_sequence = metadata.release.sequence;
    save_history(cache, history);
    snapshot(pending, metadata.artifact, cache, MaxArchive, "archive.tar.gz");
    remove_directory(cache, "candidate");
    unpack(cache.path() / "archive.tar.gz", cache.path() / "candidate", metadata);
    ProtectedDirectory candidate(cache.path() / "candidate");
    if (read_file(candidate.path() / "assets/update-public.pem", 16384) !=
            read_file(cache.path() / "public.pem", 16384) ||
        channel(candidate.path()) != GitHubChannel)
        throw std::runtime_error("Release cannot change its update trust or GitHub repository");
    {
        ProtectedDirectory native(candidate.path() / "compatibility");
        const auto signed_metadata = read_record(cache, "latest.json", MaxMetadata);
        const auto signature = read_record(cache, "latest.sig", 64);
        const auto key = read_record(cache, "public.pem", 16384);
        if (!signed_metadata || !signature || !key)
            throw std::runtime_error("Signed repair evidence is unavailable");
        record_repair_evidence(native, *signed_metadata, *signature, *key, metadata.release);
    }
    const auto old = journal(cache);
    const auto backup = directory_identity(cache, "previous-native");
    if (backup &&
        (!old || !old->healthy || backup != old->previous ||
         directory_identity(parent, installation.components.helpers.filename()) != old->next))
        throw std::runtime_error("Preserve the changed native update backup for review");
    if (backup)
        remove_directory(cache, "previous-native", old->previous);
    const auto next = *directory_identity(candidate, "compatibility");
    NativeJournal record{false, metadata.release,
                         *directory_identity(parent, installation.components.helpers.filename()),
                         next, sha256(candidate.path() / "controller-utils")};
    try {
        save(cache, record);
        move_directory(candidate, "compatibility", cache, "previous-native", RENAME_NOREPLACE);
        move_directory(parent, installation.components.helpers.filename(), cache, "previous-native",
                       RENAME_EXCHANGE);
        if (application_transaction(installation, lease.get(), true, false) != 0 ||
            !(release(installation.components.application) == record.release) ||
            sha256(installation.components.application / "controller-utils") != record.application)
            throw std::runtime_error("The complete release could not be installed");
        erase_record(cache, "archive.tar.gz");
        remove_directory(cache, "candidate");
        std::cout << "Installed Controller Utils " << metadata.release.version
                  << ". Awaiting startup confirmation.\n";
    } catch (...) {
        const auto failure = std::current_exception();
        history.failed_sequence = metadata.release.sequence;
        save_history(cache, history);
        recover(installation, cache, parent, lease.get());
        std::rethrow_exception(failure);
    }
}
} // namespace ct::update
