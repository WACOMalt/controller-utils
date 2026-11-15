#pragma once

namespace ct {
class CurlLibrary {
  public:
    CurlLibrary();
    CurlLibrary(const CurlLibrary &) = delete;
    CurlLibrary &operator=(const CurlLibrary &) = delete;
    ~CurlLibrary();
};
} // namespace ct
