#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>

int main(int, char **argv) {
    try {
        const auto path =
            std::filesystem::canonical("/proc/self/exe").parent_path() / "app/controller-utils";
        argv[0] = const_cast<char *>(path.c_str());
        execv(path.c_str(), argv);
        throw std::system_error(errno, std::generic_category(), "Run Controller Utils");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
