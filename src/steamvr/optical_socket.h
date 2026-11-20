#pragma once
#include "protocol.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstddef>
#include <cstdio>

namespace ct::optical {
// Per-user abstract sockets disappear when their providers exit.
inline socklen_t address(sockaddr_un &a, const char *channel = "optical") {
    a = {};
    a.sun_family = AF_UNIX;
    const int n = std::snprintf(a.sun_path + 1, sizeof(a.sun_path) - 1, "controller-utils.%s.v1.%u",
                                channel, unsigned(getuid()));
    return socklen_t(offsetof(sockaddr_un, sun_path) + 1 + n);
}
inline bool same_user(int fd) {
    ucred cred{};
    socklen_t size = sizeof(cred);
    return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) && size == sizeof(cred) &&
           cred.uid == getuid();
}
struct Packet {
    uint32_t version = 1;
    char serial[20]{};
    wire::AppInput input{};
};
} // namespace ct::optical
