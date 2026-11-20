#include "steamvr/ui_gate.h"
#include "platform/linux/files.h"
#include "steamvr/compatibility.h"
#include <iostream>

int main(int argc, char **argv) {
    using ct::steamvr::GateKind;
    using namespace ct::steamvr::compatibility;
    if (argc != 1 && argc != 3)
        return 2;
    try {
        for (const auto &[kind, path] :
             {std::pair{GateKind::Beam, argc == 3 ? argv[1] : Compositor.path},
              std::pair{GateKind::Input, argc == 3 ? argv[2] : SteamWebHelper.path}}) {
            try {
                ct::steamvr::resolve_gate_layout(kind, ct::read_file(path, 32 * 1024 * 1024));
            } catch (const std::exception &error) {
                throw std::runtime_error(std::string(path) + ": " + error.what());
            }
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
