#pragma once
#include <cstring>
#include <sys/utsname.h>
extern "C" int __real_uname(struct utsname *);
extern "C" int __wrap_uname(struct utsname *value) {
    const auto result = __real_uname(value);
    if (!result)
        std::strcpy(value->machine, "aarch64");
    return result;
}
