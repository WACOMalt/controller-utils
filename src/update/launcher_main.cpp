#include "launcher.h"
#include <iostream>
#include <sys/stat.h>

int main(int argc, char **argv) {
    try {
        umask(0077);
        const auto base = std::filesystem::canonical("/proc/self/exe").parent_path();
        const std::vector<std::string> arguments(argv + 1, argv + argc);
        return ct::update::launch(base / "app", base / "app-data", arguments);
    } catch (const std::exception &error) {
        std::cerr << "Controller Utils launcher: " << error.what() << '\n';
        return 1;
    }
}
