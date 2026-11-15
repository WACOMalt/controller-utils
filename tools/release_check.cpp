#include "update/archive.h"
#include "update/compatibility.h"
#include "tracking/backend.h"
#include "update/profiles.h"
#include "platform/linux/files.h"
#include <chrono>
#include <iostream>

int main(int argc, char **argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--profile-asset") {
            auto value = ct::json::parse(ct::read_file(argv[2], ct::update::MaxMetadata));
            std::cout << ct::update::profile_asset(value.get()) << '\n';
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--profile-record-history") {
            const auto profile = ct::update::verify_profile_record(
                ct::read_file(argv[2], ct::update::MaxProfile + 64), ct::read_file(argv[3], 16384),
                0);
            std::cout << profile.sequence << '\n';
            return 0;
        }
        if (argc == 5 &&
            (std::string(argv[1]) == "--profile" || std::string(argv[1]) == "--profile-history")) {
            const auto now =
                std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
            const auto profile = ct::update::verify_profile(
                ct::read_file(argv[2], ct::update::MaxProfile), ct::read_file(argv[3], 64),
                ct::read_file(argv[4], 16384),
                std::string(argv[1]) == "--profile-history" ? 0 : now);
            auto value = ct::json::parse("{}");
            json_object_object_add(value.get(), "sequence",
                                   json_object_new_uint64(profile.sequence));
            json_object_object_add(value.get(), "expires", json_object_new_uint64(profile.expires));
            std::cout << ct::json::encode(value.get()) << '\n';
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--metadata") {
            const auto metadata = ct::update::verify_metadata(argv[2], argv[3], argv[4]);
            std::cout << ct::json::encode(metadata.release.to_json().get()) << '\n';
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--requirements") {
            const ct::update::Installation installation{std::filesystem::canonical(argv[2])};
            auto requirements = ct::update::capture_requirements(
                installation, ct::installed_backend(installation.helpers));
            if (requirements.machine != "aarch64")
                throw std::runtime_error("Capture release compatibility on Steam Frame");
            std::cout << ct::json::encode(requirements.value.get()) << '\n';
            return 0;
        }
        if (argc != 4)
            throw std::invalid_argument(
                "usage: release_check FEED PUBLIC_KEY EMPTY_DESTINATION | --requirements APP");
        const std::filesystem::path feed(argv[1]);
        const auto metadata =
            ct::update::verify_metadata(feed / "latest.json", feed / "latest.sig", argv[2]);
        ct::update::unpack(feed / metadata.artifact, argv[3], metadata);
        std::cout << ct::json::encode(metadata.release.to_json().get()) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Release check: " << error.what() << '\n';
        return 1;
    }
}
