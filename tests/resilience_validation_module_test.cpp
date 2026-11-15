#include "steamvr/module.h"
#include <cassert>
#include <cstdio>
#include <dlfcn.h>
#include <filesystem>

int main(int argc, char **argv) {
    assert(argc == 1 || argc == 2);
    const auto path = std::filesystem::absolute(argc == 2 ? argv[1] : "build/fake_module.so");
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    assert(handle);
    void *function = dlsym(handle, "fixture_function");
    void *data = dlsym(handle, "fixture_data");
    assert(function && data);
    auto module = ct::steamvr::Module::loaded(function, path.c_str());
    assert(module.contains(function, 1, true));
    assert(module.contains(data, sizeof(int)));
    assert(!module.contains(data, sizeof(int), true));
    assert(!module.contains(reinterpret_cast<void *>(&main), 1));
    auto rejects = [](const void *address, const char *path) {
        try {
            (void)ct::steamvr::Module::loaded(address, path);
            return false;
        } catch (const std::runtime_error &) {
            return true;
        }
    };
    assert(rejects(function, "/wrong/fixture.so"));
    assert(rejects(function, "fixture.so"));
    assert(rejects(reinterpret_cast<void *>(&main), path.c_str()));
    assert(rejects(nullptr, path.c_str()));
    assert(dlclose(handle) == 0);
    std::puts("loaded module validation passed");
}
