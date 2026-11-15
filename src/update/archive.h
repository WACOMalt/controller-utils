#pragma once
#include "metadata.h"
#include <array>

namespace ct::update {
inline constexpr std::array<const char *, 13> Assets{"controller.stl",
                                                     "controller.surface",
                                                     "controller.surface.json",
                                                     "controller.buttons",
                                                     "controller.buttons.json",
                                                     "controller-icon.png",
                                                     "controller-icon.svg",
                                                     "controller-icon-license.txt",
                                                     "controller.json",
                                                     "README.md",
                                                     "license.txt",
                                                     "update-channel.json",
                                                     "update-public.pem"};
inline constexpr std::array<const char *, 8> PackageFiles{
    "docs/controller-utils.png",
    "controller-utils-launcher",
    "install.sh",
    "uninstall.sh",
    "README.md",
    "packaging/controller-utils-update.service",
    "packaging/controller-utils-updater.sudoers",
    "packaging/70-controller-utils.rules"};
void require_space(const std::filesystem::path &directory, uint64_t bytes);
bool allowed_member(const std::string &name);
void unpack(const std::filesystem::path &archive, const std::filesystem::path &destination,
            const Metadata &expected);
} // namespace ct::update
