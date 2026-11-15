#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <netdb.h>
#include <mutex>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

extern "C" int getaddrinfo(const char *name, const char *service, const struct addrinfo *hints,
                           struct addrinfo **result) {
    if (name && !std::strcmp(name, "controller-utils-dns-test.invalid")) {
        static std::mutex mutex;
        std::lock_guard lock(mutex);
        if (const auto path = std::getenv("CT_DNS_TEST_STARTED")) {
            const int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
            if (descriptor >= 0) {
                char message[32];
                const auto count = std::snprintf(message, sizeof(message), "%d", getpid());
                const auto written = write(descriptor, message, size_t(count));
                close(descriptor);
                if (written != count)
                    return EAI_SYSTEM;
            }
        }
        if (const auto value = std::getenv("CT_DNS_TEST_FORBIDDEN_FD"))
            if (fcntl(std::atoi(value), F_GETFD) >= 0)
                _exit(43);
        if (std::getenv("CT_DNS_TEST_EXIT"))
            _exit(42);
        timespec delay{3, 500000000};
        while (nanosleep(&delay, &delay) && errno == EINTR) {
        }
        return EAI_AGAIN;
    }
    using Resolve =
        int (*)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
    const auto resolve = reinterpret_cast<Resolve>(dlsym(RTLD_NEXT, "getaddrinfo"));
    if (resolve && name &&
        (std::strcmp(name, "github.com") == 0 ||
         std::strcmp(name, "release-assets.githubusercontent.com") == 0))
        if (const auto port = std::getenv("CT_HTTPS_TEST_PORT"))
            return resolve("127.0.0.1", port, hints, result);
    return resolve ? resolve(name, service, hints, result) : EAI_SYSTEM;
}
