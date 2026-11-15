#pragma once
#include "presentation.h"
#include "steamvr/runtime.h"
#include <filesystem>
#include <memory>

namespace ct::ui {
class Renderer {
    class Impl;
    std::unique_ptr<Impl> implementation_;

  public:
    Renderer(Runtime &runtime, Presentation &presentation, const std::filesystem::path &root);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    void run();
};
} // namespace ct::ui
