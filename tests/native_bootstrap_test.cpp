#include "helper/bootstrap.h"
#include "patch_memory.h"
#include <cassert>
#include <iostream>

namespace {
template <class Function> void rejects(Function fn, const std::string &text = "") {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &error) {
        failed = std::string(error.what()).find(text) != std::string::npos;
    }
    assert(failed);
}
void seed(ct::test::PatchMemory &memory) {
    memory.regions[ct::helper::CameraPatch::Address] = ct::helper::CameraPatch::Original;
}
} // namespace
int main() {
    using namespace ct::helper;
    for (bool after : {false, true}) {
        for (unsigned write = 1; write <= 2; ++write) {
            ct::test::PatchMemory memory;
            seed(memory);
            const auto original = memory.regions;
            memory.fail_at = write;
            memory.fail_after = after;
            CameraPatch camera(memory);
            rejects([&] { camera.apply(); }, "injected patch write failure");
            assert(memory.regions == original);
        }
    }
    for (unsigned parts = 0; parts < 4; ++parts) {
        ct::test::PatchMemory memory;
        seed(memory);
        auto &bytes = memory.regions[CameraPatch::Address];
        for (size_t word = 0; word < 2; ++word)
            if (parts & (1u << word))
                std::copy(CameraPatch::Corrected.begin() + word * 4,
                          CameraPatch::Corrected.begin() + word * 4 + 4, bytes.begin() + word * 4);
        CameraPatch camera(memory);
        camera.restore();
        assert(bytes == CameraPatch::Original);
        const auto writes = memory.writes;
        camera.restore();
        assert(memory.writes == writes);
        camera.apply();
        assert(bytes == CameraPatch::Corrected);
        bytes[1] ^= 1;
        const auto before = memory.regions;
        rejects([&] { camera.restore(); }, "changed externally");
        assert(before == memory.regions);
    }
    std::cout << "native camera restoration checks passed\n";
}
