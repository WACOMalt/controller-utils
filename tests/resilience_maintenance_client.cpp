#include "update/client.h"
#include "platform/linux/files.h"
#include "update_fixture.h"
#include <cassert>
#include <chrono>
#include <thread>
#include <iostream>

using namespace ct;
using namespace ct::update;
using namespace std::chrono_literals;
template <class F> void wait_for(F f) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!f()) {
        if (std::chrono::steady_clock::now() >= end)
            throw std::runtime_error("worker timed out");
        std::this_thread::sleep_for(2ms);
    }
}
int main() {
    const auto root = std::filesystem::canonical("/proc/self/exe").parent_path() /
                      ("resilience-client-" + std::to_string(getpid()));
    std::filesystem::create_directories(root / "app/assets");
    std::filesystem::create_directory(root / "data");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    atomic_write(root / "app/release.json",
                 R"({"version":"0.1.0","sequence":1,"platform":"linuxarm64"})");
    atomic_write(root / "app/assets/update-channel.json",
                 R"({"url":"https://updates.example.test/"})");
    auto key = update_fixture::keypair();
    auto expected = update_fixture::requirements();
    atomic_write(root / "app/assets/update-public.pem", update_fixture::public_key(key.get()));
    std::filesystem::create_directory(root / "feed");
    const auto artifact = update_fixture::make_archive(
        root / "feed/controller-utils-fixture-linuxarm64.tar.gz", update_fixture::payload());
    auto manifest = artifact.release.to_json();
    json_object_object_add(manifest.get(), "schema", json_object_new_int(5));
    json_object_object_add(manifest.get(), "expires", json_object_new_int(0));
    json_object_object_add(manifest.get(), "artifact",
                           json_object_new_string(artifact.artifact.c_str()));
    json_object_object_add(manifest.get(), "size", json_object_new_uint64(artifact.size));
    json_object_object_add(manifest.get(), "sha256",
                           json_object_new_string(artifact.sha256.c_str()));
    json_object_object_add(manifest.get(), "notes", json_object_new_string("fixture"));
    json_object_object_add(manifest.get(), "requirements", json_object_get(expected.get()));
    const auto manifest_bytes = json::encode(manifest.get());
    atomic_write(root / "feed/latest.json", manifest_bytes);
    update_fixture::sign(root / "feed/latest.sig", key.get(), manifest_bytes);
    std::string profile_bytes, profile_sig;
    {
        json::Value profile(json_object_new_object());
        json_object_object_add(profile.get(), "schema", json_object_new_int(2));
        json_object_object_add(profile.get(), "sequence", json_object_new_int(1));
        json_object_object_add(profile.get(), "expires", json_object_new_int(200000));
        json_object_object_add(profile.get(), "platform", json_object_new_string("linuxarm64"));
        json_object_object_add(profile.get(), "validator", json_object_new_string("queue-v2"));
        json_object_object_add(profile.get(), "backend", json_object_new_string("native-tracker"));
        auto queue = json_object_new_object();
        auto fingerprints = json_object_new_array();
        for (unsigned i = 0; i < 9; ++i)
            json_object_array_add(fingerprints,
                                  json_object_new_string(std::string(64, 'b').c_str()));
        json_object_object_add(queue, "fingerprints", fingerprints);
        json_object_object_add(profile.get(), "queue", queue);
        profile_bytes = json::encode(profile.get());
        update_fixture::sign(root / "feed/profile.sig", key.get(), profile_bytes);
        profile_sig = read_file(root / "feed/profile.sig", 64);
    }
    std::atomic_int app_calls{0}, profile_calls{0}, mode{0};
    std::atomic_bool waiting{false}, cancelled{false};
    ProfileDownload transport = [&](const std::string &url, const std::filesystem::path &path,
                                    size_t limit, const std::atomic_bool &cancel) {
        if (url.find(ProfileBase) != 0 && url != ProfileIndex) {
            ++app_calls;
            if (mode == 3) {
                const auto name = url.substr(url.find_last_of('/') + 1);
                const auto bytes = read_file(root / "feed" / name, limit);
                atomic_write(path, bytes, 0600);
                return;
            }
            throw std::runtime_error("HTTP 404 synthetic application feed");
        }
        ++profile_calls;
        if (mode == 0 || mode == 3)
            throw std::runtime_error("HTTP 404 synthetic profile feed");
        if (mode == 2) {
            waiting = true;
            wait_for([&] { return cancel.load(); });
            cancelled = true;
            throw DownloadCancelled();
        }
        if (url == ProfileIndex) {
            atomic_write(
                path,
                R"({"assets":[{"name":"compatibility-profile-1.bin","state":"uploaded","size":1024}]})",
                0600);
            return;
        }
        assert(limit == MaxProfile + 64);
        atomic_write(path, mode == 4 ? profile_sig + profile_bytes : std::string(65, 'x'), 0600);
    };
    UpdateClient client(
        root / "app", root / "data",
        [&] { return InstalledRequirements{"aarch64", json::parse(json::encode(expected.get()))}; },
        {}, [] { return 100000.; }, transport, update_fixture::public_key(key.get()));
    const auto stored_profile = [&] {
        return load_profile(ProtectedDirectory(root / "data/updates", getuid()),
                            update_fixture::public_key(key.get()), 100000.);
    };
    for (int i = 0; i != 10; ++i)
        (void)client.poll(false);
    assert(app_calls == 0 && profile_calls == 0);
    (void)client.poll(true);
    wait_for(
        [&] { return !client.poll(false).busy && profile_calls >= 1 && !client.profile_busy(); });
    assert(app_calls == 1 && client.poll(false).message == "No releases have been published yet.");
    for (int i = 0; i != 5; ++i)
        (void)client.poll(true);
    assert(app_calls == 1 && profile_calls == 1);
    mode = 1;
    client.check(true);
    wait_for(
        [&] { return !client.poll(false).busy && profile_calls >= 3 && !client.profile_busy(); });
    assert(app_calls == 2 && !stored_profile());
    mode = 3;
    client.check(true);
    wait_for([&] {
        return !client.poll(false).busy && app_calls >= 5 &&
               std::filesystem::exists(root / "data/updates/pending");
    });
    assert(client.poll(false).message ==
           "Version 0.1.1 is ready. It will install when SteamVR next starts.");
    wait_for([&] { return !client.profile_busy(); });
    {
        mode = 4;
        const auto prior_calls = profile_calls.load();
        client.check(true);
        wait_for([&] { return profile_calls >= prior_calls + 2 && !client.profile_busy(); });
        assert(stored_profile() && stored_profile()->sequence == 1);
        UpdateClient restarted(
            root / "app", root / "data",
            [&] {
                return InstalledRequirements{"aarch64", json::parse(json::encode(expected.get()))};
            },
            {}, [] { return 100000.; }, transport, update_fixture::public_key(key.get()));
        assert(stored_profile() && stored_profile()->sequence == 1);
        restarted.close();
    }
    mode = 1;
    const auto before_invalid = profile_calls.load();
    client.check(true);
    wait_for([&] {
        return profile_calls >= before_invalid + 2 && !client.profile_busy() &&
               !client.poll(false).busy;
    });
    assert(stored_profile() && stored_profile()->sequence == 1);
    mode = 2;
    const auto previous_app_calls = app_calls.load();
    client.check(true);
    wait_for([&] { return waiting.load(); });
    const auto count = profile_calls.load();
    client.check(true);
    wait_for([&] { return app_calls >= previous_app_calls + 2 && !client.poll(false).busy; });
    assert(profile_calls == count);
    const auto start = std::chrono::steady_clock::now();
    client.close();
    assert(cancelled && std::chrono::steady_clock::now() - start < 2s);
    assert(app_calls == previous_app_calls + 2);
    std::cout << "client: automatic preference, manual checks, isolated failures, concurrency and "
                 "cancellation passed\n";
}
