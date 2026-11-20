#include "frame_update_source.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

int main(int argc, char **argv) {
    try {
        if (getuid() != 1000 || geteuid() != 1000 || argc > 2 ||
            (argc == 2 && std::string(argv[1]) != "--background" &&
             std::string(argv[1]) != "--show"))
            throw std::runtime_error("Unexpected legacy migration launcher arguments");
        const auto root = std::filesystem::canonical("/proc/self/exe").parent_path();
        execl("/usr/bin/systemd-run", "systemd-run", "--user", "--quiet", "--collect", "--no-block",
              "--expand-environment=no", "--unit=controller-utils-frame-migration",
              "/usr/bin/python3", "-B", "-c", frame_update_source, "migrate", root.c_str(),
              nullptr);
        throw std::system_error(errno, std::generic_category(), "start Frame migration");
    } catch (const std::exception &error) {
        std::cerr << "Controller Utils: " << error.what() << '\n';
        return 1;
    }
}
