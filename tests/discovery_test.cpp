#include "helper/discovery.h"
#include "platform/linux/child.h"
#include "platform/linux/error.h"
#include "process_image.h"
#include <cassert>
#include <iostream>

namespace {
void ready(ct::ChildProcess &child) {
    const auto deadline = ct::monotonic_time() + 2;
    while (child.poll().empty()) {
        assert(ct::monotonic_time() < deadline);
        ct::sleep_for(.001);
    }
}
template <class Function> void rejects(Function fn, bool transient) {
    bool failed = false;
    try {
        fn();
    } catch (const ct::ConnectionError &) {
        assert(transient);
        failed = true;
    } catch (const std::exception &) {
        assert(!transient);
        failed = true;
    }
    assert(failed);
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--target") {
        std::cout << "{\"op\":\"ready\"}\n" << std::flush;
        std::string command;
        std::getline(std::cin, command);
        return 0;
    }
    ct::test::ProcessImage image("controller-utils-discovery-" + std::to_string(getpid()));
    ct::ChildProcess first({image.launch(), "--target"}, std::filesystem::current_path());
    ready(first);
    const auto spec = image.spec(first.pid());
    const auto path = spec.executable.string();
    const ct::steamvr::compatibility::Binary binary{"discovery-test", path.c_str(),
                                                    spec.sha256.c_str()};
    assert(ct::helper::find_process(binary, false, getuid()) == first.pid());
    rejects([&] { ct::helper::find_process(binary, false, getuid() + 1); }, true);
    rejects([&] { ct::helper::find_process(binary, true, getuid()); }, true);
    {
        ct::ChildProcess second({image.launch(), "--target"}, std::filesystem::current_path());
        ready(second);
        rejects([&] { ct::helper::find_process(binary, false, getuid()); }, false);
        second.close("q\n");
    }
    assert(ct::helper::find_process(binary, false, getuid()) == first.pid());
    first.close("q\n");
    rejects([&] { ct::helper::find_process(binary, false, getuid()); }, true);
    std::cout << "native discovery: exact executable, owner, VR association, duplicates and "
                 "process exit passed\n";
}
