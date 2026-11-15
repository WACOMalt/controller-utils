#include "download_worker.h"
#include "platform/curl.h"
#include "platform/linux/resource.h"
#include "metadata.h"
#include "platform/linux/error.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <curl/curl.h>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <sys/stat.h>

namespace ct::update {
namespace {
void check(CURLcode code) {
    if (code != CURLE_OK)
        throw std::runtime_error(std::string("Update download failed: ") +
                                 curl_easy_strerror(code));
}
void check(CURLMcode code) {
    if (code != CURLM_OK)
        throw std::runtime_error(std::string("Update download failed: ") +
                                 curl_multi_strerror(code));
}
struct Handles {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy{curl_easy_init(), curl_easy_cleanup};
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi{curl_multi_init(),
                                                                curl_multi_cleanup};
    bool attached = false;
    Handles() {
        if (!easy || !multi)
            throw std::bad_alloc();
    }
    void attach() {
        check(curl_multi_add_handle(multi.get(), easy.get()));
        attached = true;
    }
    void detach() {
        check(curl_multi_remove_handle(multi.get(), easy.get()));
        attached = false;
    }
    ~Handles() {
        if (attached)
            curl_multi_remove_handle(multi.get(), easy.get());
    }
};
struct Transfer {
    int output;
    uint64_t limit, written = 0;
    const std::function<double()> &clock;
    std::exception_ptr failure;
    std::optional<uint64_t> length;
    double activity;
    size_t header_bytes = 0, discarded = 0;
    unsigned status = 0;
    std::string location;
    size_t bytes(size_t size, size_t count) {
        if (size && count > SIZE_MAX / size)
            throw std::invalid_argument("Update download exceeded its size limit");
        return size * count;
    }
    static size_t body(char *data, size_t size, size_t count, void *context) noexcept {
        auto &self = *static_cast<Transfer *>(context);
        try {
            const auto total = self.bytes(size, count);
            if (self.status >= 300 && self.status < 400) {
                if (total > MaxMetadata - self.discarded)
                    throw std::invalid_argument("Update redirect exceeded its size limit");
                self.discarded += total;
                self.activity = self.clock();
                return total;
            }
            if (total > self.limit - self.written)
                throw std::invalid_argument("Update download exceeded its size limit");
            size_t used = 0;
            while (used < total) {
                const auto n = write(self.output, data + used, total - used);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    throw std::system_error(n < 0 ? errno : EIO, std::generic_category(),
                                            "write update download");
                used += size_t(n);
            }
            self.written += total;
            self.activity = self.clock();
            return total;
        } catch (...) {
            self.failure = std::current_exception();
            return 0;
        }
    }
    static size_t header(char *data, size_t size, size_t count, void *context) noexcept {
        auto &self = *static_cast<Transfer *>(context);
        try {
            const auto total = self.bytes(size, count);
            if (total > MaxMetadata - self.header_bytes)
                throw std::invalid_argument("Update response header exceeded its size limit");
            self.header_bytes += total;
            std::string_view line(data, total);
            if (line.substr(0, 5) == "HTTP/") {
                const auto space = line.find(' ');
                if (space == std::string_view::npos || line.size() < space + 4)
                    throw std::invalid_argument("Invalid update HTTP response");
                unsigned code = 0;
                const auto parsed =
                    std::from_chars(line.data() + space + 1, line.data() + space + 4, code);
                if (parsed.ec != std::errc{} || parsed.ptr != line.data() + space + 4)
                    throw std::invalid_argument("Invalid update HTTP response");
                self.status = code;
                self.location.clear();
                self.length.reset();
            } else {
                constexpr std::string_view redirect_name = "location:";
                if (line.size() >= redirect_name.size() &&
                    std::equal(redirect_name.begin(), redirect_name.end(), line.begin(),
                               [](unsigned char a, unsigned char b) {
                                   return a == b || (b >= 'A' && b <= 'Z' && a == b - 'A' + 'a');
                               })) {
                    auto location = line.substr(redirect_name.size());
                    while (!location.empty() &&
                           (location.front() == ' ' || location.front() == '\t'))
                        location.remove_prefix(1);
                    while (!location.empty() &&
                           (location.back() == '\r' || location.back() == '\n' ||
                            location.back() == ' ' || location.back() == '\t'))
                        location.remove_suffix(1);
                    if (location.empty() || !self.location.empty())
                        throw std::invalid_argument("Invalid update redirect header");
                    self.location = location;
                }
                constexpr std::string_view name = "content-length:";
                const bool length =
                    line.size() >= name.size() &&
                    std::equal(name.begin(), name.end(), line.begin(),
                               [](unsigned char a, unsigned char b) {
                                   return a == b || (b >= 'A' && b <= 'Z' && a == b - 'A' + 'a');
                               });
                if (length) {
                    line.remove_prefix(name.size());
                    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                        line.remove_prefix(1);
                    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                                             line.back() == '\r' || line.back() == '\n'))
                        line.remove_suffix(1);
                    uint64_t value = 0;
                    const auto parsed =
                        std::from_chars(line.data(), line.data() + line.size(), value);
                    if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size() ||
                        value > ((self.status >= 300 && self.status < 400) ? MaxMetadata
                                                                           : self.limit) ||
                        (self.length && *self.length != value))
                        throw std::invalid_argument(
                            "Update download exceeded its size limit or has an invalid length");
                    self.length = value;
                }
            }
            self.activity = self.clock();
            return total;
        } catch (...) {
            self.failure = std::current_exception();
            return 0;
        }
    }
};
std::string redirect(const std::string &original, const std::string &current,
                     const std::string &next) {
    using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
    auto parse = [](const std::string &value) {
        Url result(curl_url(), curl_url_cleanup);
        if (!result || value.find_first_of("\r\n\t ") != std::string::npos ||
            curl_url_set(result.get(), CURLUPART_URL, value.c_str(), CURLU_DISALLOW_USER))
            throw std::invalid_argument("Invalid update redirect");
        return result;
    };
    auto first = parse(original), target = parse(current);
    if (next.find_first_of("\r\n\t ") != std::string::npos ||
        curl_url_set(target.get(), CURLUPART_URL, next.c_str(), CURLU_DISALLOW_USER))
        throw std::invalid_argument("Invalid update redirect");
    auto part = [](CURLU *url, CURLUPart item) {
        char *data = nullptr;
        const auto result = curl_url_get(url, item, &data, CURLU_DEFAULT_PORT);
        std::unique_ptr<char, decltype(&curl_free)> owned(data, curl_free);
        return result ? std::string{} : std::string(data);
    };
    const auto host = part(target.get(), CURLUPART_HOST);
    const bool same = host == part(first.get(), CURLUPART_HOST) &&
                      part(target.get(), CURLUPART_PORT) == part(first.get(), CURLUPART_PORT);
    const bool github =
        part(first.get(), CURLUPART_HOST) == "github.com" &&
        part(first.get(), CURLUPART_PATH).rfind("/kallupx/controller-utils/releases/", 0) == 0 &&
        part(first.get(), CURLUPART_PORT) == "443";
    const bool destination =
        part(target.get(), CURLUPART_PORT) == "443" &&
        ((host == "github.com" &&
          part(target.get(), CURLUPART_PATH).rfind("/kallupx/controller-utils/releases/", 0) == 0 &&
          part(target.get(), CURLUPART_QUERY).empty()) ||
         host == "release-assets.githubusercontent.com" || host == "objects.githubusercontent.com");
    if (part(target.get(), CURLUPART_SCHEME) != "https" ||
        !part(target.get(), CURLUPART_FRAGMENT).empty() || (github ? !destination : !same))
        throw std::invalid_argument("Untrusted update download redirect");
    return part(target.get(), CURLUPART_URL);
}
} // namespace
uint64_t download_to_descriptor(const std::string &url, int output, uint64_t limit,
                                const std::filesystem::path &certificates) {
    CurlLibrary library;
    const std::function<double()> monotonic = monotonic_time;
    Handles handles;
    const auto easy = handles.easy.get();
    const auto started = monotonic();
    Transfer transfer{output, limit, 0, monotonic, {}, std::nullopt, started, 0, 0, 0, {}};
    check(curl_easy_setopt(easy, CURLOPT_URL, url.c_str()));
    check(curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https"));
    check(curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https"));
    check(curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L));
    check(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L));
    check(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L));
    check(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L));
    check(curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 3000L));
    check(curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 120000L));
    check(curl_easy_setopt(easy, CURLOPT_HTTP_CONTENT_DECODING, 0L));
    check(curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L));
    check(curl_easy_setopt(easy, CURLOPT_USERAGENT, "Controller-Utils-Updater/1"));
    check(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, Transfer::body));
    check(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &transfer));
    check(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, Transfer::header));
    check(curl_easy_setopt(easy, CURLOPT_HEADERDATA, &transfer));
    if (!certificates.empty())
        check(curl_easy_setopt(easy, CURLOPT_CAINFO, certificates.c_str()));
    std::string address = url;
    for (unsigned hop = 0; hop <= 5; ++hop) {
        check(curl_easy_setopt(easy, CURLOPT_URL, address.c_str()));
        handles.attach();
        CURLcode result = CURLE_FAILED_INIT;
        for (;;) {
            if (monotonic() - started >= 120)
                throw TimeoutError("Update download timed out");
            if (monotonic() - transfer.activity >= 3)
                throw TimeoutError("Update download timed out");
            int active = 0;
            check(curl_multi_perform(handles.multi.get(), &active));
            if (transfer.failure)
                std::rethrow_exception(transfer.failure);
            int pending = 0;
            if (auto message = curl_multi_info_read(handles.multi.get(), &pending)) {
                if (message->msg != CURLMSG_DONE || message->easy_handle != easy)
                    throw std::runtime_error("Unexpected native download event");
                result = message->data.result;
                break;
            }
            if (!active)
                throw std::runtime_error("Native download stopped without a result");
            check(curl_multi_poll(handles.multi.get(), nullptr, 0, 50, nullptr));
        }
        long status = 0;
        check(curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status));
        handles.detach();
        if (status == 404)
            throw std::runtime_error("Update download returned HTTP 404");
        check(result);
        if (status >= 300 && status < 400) {
            if (transfer.location.empty() || hop == 5)
                throw std::runtime_error("Update download exceeded its redirect limit");
            address = redirect(url, address, transfer.location);
            transfer.length.reset();
            continue;
        }
        if (status < 200 || status >= 300 ||
            (transfer.length && transfer.written != *transfer.length))
            throw std::runtime_error("Invalid or incomplete update HTTP response");
        return transfer.written;
    }
    throw std::runtime_error("Update download exceeded its redirect limit");
}
} // namespace ct::update
