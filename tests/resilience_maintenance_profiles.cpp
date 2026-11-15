#include "update/profiles.h"
#include "update_fixture.h"
#include <cassert>
#include <iostream>

using namespace ct;
using namespace ct::update;
using namespace update_fixture;
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
std::string profile(unsigned sequence, unsigned expires = 2000) {
    json::Value root(json_object_new_object());
    json_object_object_add(root.get(), "schema", json_object_new_int(2));
    json_object_object_add(root.get(), "sequence", json_object_new_int(sequence));
    json_object_object_add(root.get(), "expires", json_object_new_int(expires));
    json_object_object_add(root.get(), "platform", json_object_new_string("linuxarm64"));
    json_object_object_add(root.get(), "validator", json_object_new_string("queue-v2"));
    json_object_object_add(root.get(), "backend", json_object_new_string("native-tracker"));
    auto queue = json_object_new_object();
    auto hashes = json_object_new_array();
    for (int i = 0; i != 9; ++i)
        json_object_array_add(hashes,
                              json_object_new_string(std::string(64, char('a' + i % 6)).c_str()));
    json_object_object_add(queue, "fingerprints", hashes);
    json_object_object_add(root.get(), "queue", queue);
    return json::encode(root.get());
}
int main() {
    auto index = json::parse(
        R"({"assets":[{"name":"compatibility-profile-1.bin","state":"uploaded","size":1024},{"name":"compatibility-profile-2.bin","state":"starter","size":0}]})");
    assert(profile_asset(index.get()) == "compatibility-profile-1.bin");
    for (
        const auto text :
        {R"({"assets":[]})",
         R"({"assets":[{"name":"compatibility-profile-01.bin","state":"uploaded","size":100}]})",
         R"({"assets":[{"name":"compatibility-profile-1.bin","state":"uploaded","size":64}]})",
         R"({"assets":[{"name":"compatibility-profile-1.bin","state":"uploaded","size":100},{"name":"compatibility-profile-1.bin","state":"uploaded","size":100}]})"})
        rejects([&] {
            auto value = json::parse(text);
            profile_asset(value.get());
        });
    const auto root = std::filesystem::canonical("/proc/self/exe").parent_path() /
                      ("resilience-profile-" + std::to_string(getpid()));
    assert(std::filesystem::create_directory(root));
    auto key = keypair(), other = keypair();
    auto pub = public_key(key.get());
    auto sign_bytes = [&](const std::string &s) {
        sign(root / "signature", key.get(), s);
        return read_file(root / "signature", 64);
    };
    auto first = profile(1), sig = sign_bytes(first);
    assert(verify_profile(first, sig, pub, 1000).sequence == 1);
    rejects([&] { verify_profile(first, sig, public_key(other.get()), 1000); });
    rejects([&] { verify_profile(first + " ", sig, pub, 1000); });
    rejects([&] { verify_profile(first, sig.substr(1), pub, 1000); });
    rejects([&] { verify_profile(first, sig, pub, 2000); });
    rejects([&] { verify_profile(std::string(MaxProfile + 1, 'x'), sig, pub, 1000); });
    for (const auto &bad :
         {std::string("{\"schema\":1,") + first.substr(1),
          first.substr(0, first.size() - 1) + ",\"permission\":true}",
          first.substr(0, first.size() - 1) + ",\"code\":\"exec\"}",
          first.substr(0, first.size() - 1) + ",\"mask\":\"ffff\"}",
          first.substr(0, first.size() - 1) + ",\"url\":\"https://example.test\"}",
          std::string(first).replace(first.find("linuxarm64"), 10, "linuxamd64"),
          std::string(first).replace(first.find("queue-v2"), 8, "queue-v3"),
          std::string(first).replace(first.find("\"sequence\":1"), 12, "\"sequence\":true"),
          first.substr(0, first.size() - 1)}) {
        auto s = sign_bytes(bad);
        rejects([&] { verify_profile(bad, s, pub, 1000); });
    }
    {
        MaintenanceLock lock(root);
        assert(!load_profile(lock.cache(), pub, 1000));
        assert(accept_profile(lock, first, sig, pub, 1000).sequence == 1);
        assert(load_profile(lock.cache(), pub, 1000)->sequence == 1);
        rejects([&] { MaintenanceLock concurrent(root); });
        auto replacement = profile(1, 2100), replacement_sig = sign_bytes(replacement);
        rejects([&] { accept_profile(lock, replacement, replacement_sig, pub, 1000); });
        auto second = profile(2), second_sig = sign_bytes(second);
        assert(accept_profile(lock, second, second_sig, pub, 1000).sequence == 2);
        rejects([&] { accept_profile(lock, first, sig, pub, 1000); });
        assert(load_profile(lock.cache(), pub, 1000)->sequence == 2);
        auto third = profile(3), third_sig = sign_bytes(third);
        std::atomic_bool cancel{false};
        unsigned calls = 0;
        auto download = [&](const std::string &url, const std::filesystem::path &path, size_t limit,
                            const std::atomic_bool &) {
            ++calls;
            const bool index = url == ProfileIndex;
            assert(index || url == std::string(ProfileBase) + "compatibility-profile-3.bin");
            const auto bytes =
                index
                    ? R"({"assets":[{"name":"compatibility-profile-3.bin","state":"uploaded","size":1024},{"name":"compatibility-profile-4.bin","state":"starter","size":0}]})"
                    : third_sig + third;
            assert(bytes.size() <= limit);
            atomic_write(path, bytes, 0600);
        };
        auto commit = [&](const std::string &data, const std::string &signature) {
            accept_profile(lock, data, signature, pub, 1000);
        };
        acquire_profile(lock.cache(), download, cancel, commit);
        assert(calls == 2 && load_profile(lock.cache(), pub, 1000)->sequence == 3);
        cancel = true;
        rejects([&] { acquire_profile(lock.cache(), download, cancel, commit); });
        assert(load_profile(lock.cache(), pub, 1000)->sequence == 3);
        cancel = false;
        rejects([&] {
            acquire_profile(
                lock.cache(),
                [](const auto &, const auto &, auto, const auto &) {
                    throw std::runtime_error("synthetic transfer failure");
                },
                cancel, commit);
        });
        assert(load_profile(lock.cache(), pub, 1000)->sequence == 3);
    }
    {
        MaintenanceLock lock(root);
        assert(load_profile(lock.cache(), pub, 1000)->sequence == 3);
        atomic_write(lock.cache().path() / ".pending-interrupted", "partial", 0600);
        assert(load_profile(lock.cache(), pub, 1000)->sequence == 3);
        atomic_write(lock.cache().path() / ProfileRecord, "partial", 0600);
        rejects([&] { accept_profile(lock, first, sig, pub, 1000); });
        rejects([&] { load_profile(lock.cache(), pub, 1000); });
        std::filesystem::remove(lock.cache().path() / ProfileRecord);
        std::filesystem::create_symlink(root / "signature", lock.cache().path() / ProfileRecord);
        rejects([&] { load_profile(lock.cache(), pub, 1000); });
        rejects([&] { accept_profile(lock, first, sig, pub, 1000); });
    }
    std::filesystem::remove_all(root);
    std::cout << "signed profiles: schema, tampering, sequence, state and lock passed\n";
}
