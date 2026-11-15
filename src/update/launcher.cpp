#include "launcher.h"
#include "app/instance.h"
#include "metadata.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <elf.h>
#include <iostream>
#include <system_error>
#include <fcntl.h>

extern char **environ;
namespace ct::update {
namespace {
void show(ApplicationLock &lock, const std::filesystem::path &program) {
    const auto deadline = monotonic_time() + 15;
    while (monotonic_time() < deadline) {
        const auto started = monotonic_time();
        const auto interval = std::min(.1, deadline - started);
        if (interval <= 0)
            break;
        try {
            if (lock.show_running(program, interval))
                return;
        } catch (const std::system_error &error) {
            if (error.code().value() != ENOENT)
                throw;
        }
        const auto remaining = interval - (monotonic_time() - started);
        if (remaining > 0)
            sleep_for(remaining);
    }
    throw std::runtime_error("Controller Utils is still starting. Try opening it again.");
}
[[noreturn]] void execute(const std::filesystem::path &root, ApplicationLock &lock,
                          const std::vector<std::string> &arguments) {
    ProtectedDirectory directory(root, getuid());
    const auto program = directory.file("controller-utils", MaxContents);
    Elf64_Ehdr header{};
    size_t offset = 0;
    while (offset < sizeof(header)) {
        const auto count = pread(program.get(), reinterpret_cast<char *>(&header) + offset,
                                 sizeof(header) - offset, off_t(offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            throw std::system_error(errno, std::generic_category(), "read native application");
        if (!count)
            throw std::runtime_error("Controller Utils requires a native application binary");
        offset += size_t(count);
    }
#if defined(__aarch64__)
    constexpr auto machine = EM_AARCH64;
#elif defined(__x86_64__)
    constexpr auto machine = EM_X86_64;
#else
#error Unsupported native launcher architecture
#endif
    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
        header.e_version != EV_CURRENT || header.e_machine != machine ||
        (header.e_type != ET_EXEC && header.e_type != ET_DYN))
        throw std::runtime_error("Controller Utils requires a native application for this CPU");
    struct stat image{};
    if (fstat(program.get(), &image))
        throw std::system_error(errno, std::generic_category(), "inspect native application");
    if (!(image.st_mode & 0111))
        throw std::runtime_error("Controller Utils application is not executable");
    directory.verify_file("controller-utils", program.get());
    if (fchdir(directory.descriptor()))
        throw std::system_error(errno, std::generic_category(),
                                "enter native application directory");
    std::vector<std::string> values{(root / "controller-utils").string()};
    values.insert(values.end(), arguments.begin(), arguments.end());
    std::vector<char *> argv;
    for (auto &value : values)
        argv.push_back(value.data());
    argv.push_back(nullptr);
    lock.make_inheritable();
    const auto descriptor = std::to_string(lock.descriptor());
    if (setenv("CONTROLLER_UTILS_LOCK_FD", descriptor.c_str(), 1))
        throw std::system_error(errno, std::generic_category(), "pass native application lock");
    fexecve(program.get(), argv.data(), environ);
    throw std::system_error(errno, std::generic_category(), "start native Controller Utils");
}
} // namespace
int launch(const std::filesystem::path &application, const std::filesystem::path &data,
           const std::vector<std::string> &arguments) {
    if (arguments.size() > 1 || (!arguments.empty() && arguments.front() != "--show" &&
                                 arguments.front() != "--background"))
        throw std::invalid_argument("usage: controller-utils [--show|--background]");
    ApplicationLock lock(data / "app.lock");
    if (!lock.primary()) {
        show(lock, application / "controller-utils");
        return 0;
    }
    execute(application, lock, arguments);
}
} // namespace ct::update
