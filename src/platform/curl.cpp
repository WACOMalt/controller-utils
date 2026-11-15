#include "curl.h"
#include <curl/curl.h>
#include <stdexcept>

namespace ct {
CurlLibrary::CurlLibrary() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        throw std::runtime_error("Cannot initialize native HTTP transport");
}
CurlLibrary::~CurlLibrary() {
    curl_global_cleanup();
}
} // namespace ct
