#include <curl/curl.h>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <json-c/json.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include "steam_ui_transport.h"
#include "input_filter_js.h"

namespace ct::steamui {
using Json = json::Value;
using Curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
constexpr size_t max_message = 1024 * 1024;
double monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
Json own(json_object *value) {
    return Json(value);
}
Json parse(const std::string &text) {
    json_tokener *tok = json_tokener_new();
    if (!tok)
        throw std::runtime_error("Cannot allocate JSON parser");
    auto result = own(json_tokener_parse_ex(tok, text.c_str(), static_cast<int>(text.size())));
    const auto error = json_tokener_get_error(tok);
    size_t end = json_tokener_get_parse_end(tok);
    json_tokener_free(tok);
    while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])))
        ++end;
    if (error != json_tokener_success || !result || end != text.size())
        throw std::runtime_error("Invalid input filter JSON");
    return result;
}
json_object *member(json_object *obj, const char *name) {
    json_object *value = nullptr;
    if (obj)
        json_object_object_get_ex(obj, name, &value);
    return value;
}
std::string string(json_object *obj) {
    if (!obj || !json_object_is_type(obj, json_type_string))
        return {};
    return {json_object_get_string(obj), static_cast<size_t>(json_object_get_string_len(obj))};
}
std::string encode(json_object *obj) {
    return json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
}
void add(json_object *obj, const char *name, const std::string &value) {
    json_object_object_add(
        obj, name, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
bool serial_valid(const std::string &serial) {
    return !serial.empty() && serial.size() <= 64 &&
           std::all_of(serial.begin(), serial.end(), [](unsigned char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-';
           });
}
std::string websocket_target(json_object *targets) {
    if (!json_object_is_type(targets, json_type_array))
        throw std::runtime_error("Invalid Steam debugger target list");
    std::string result;
    for (size_t i = 0; i < json_object_array_length(targets); ++i) {
        auto item = json_object_array_get_idx(targets, i);
        const std::string url = string(member(item, "url"));
        if (string(member(item, "title")) != "SharedJSContext" ||
            url.rfind("https://steamloopback.host/", 0) != 0)
            continue;
        if (!result.empty())
            throw std::runtime_error("Steam shared debugger target is ambiguous");
        std::string ws = string(member(item, "webSocketDebuggerUrl"));
        const std::string local = "ws://localhost:8080/";
        if (ws.rfind(local, 0) == 0)
            ws.replace(0, local.size(), "ws://127.0.0.1:8080/");
        const std::string prefix = "ws://127.0.0.1:8080/devtools/page/";
        if (ws.rfind(prefix, 0) != 0 || !serial_valid(ws.substr(prefix.size())))
            throw std::runtime_error("Steam debugger websocket is not the expected loopback page");
        result = ws;
    }
    if (result.empty())
        throw std::runtime_error("Steam SharedJSContext debugger is unavailable");
    return result;
}
void configure(CURL *curl, const std::string &url) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 250L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 500L);
}
void check(CURLcode code) {
    if (code != CURLE_OK)
        throw std::runtime_error(std::string("Steam debugger transport: ") +
                                 curl_easy_strerror(code));
}
size_t http_body(char *data, size_t size, size_t count, void *pointer) {
    auto &output = *static_cast<std::string *>(pointer);
    const size_t bytes = size * count;
    if (bytes > max_message - output.size())
        return 0;
    output.append(data, bytes);
    return bytes;
}
std::string discover() {
    Curl curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        throw std::runtime_error("Cannot initialize debugger HTTP connection");
    configure(curl.get(), "http://127.0.0.1:8080/json/list");
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http");
    std::string body;
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, http_body);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    check(curl_easy_perform(curl.get()));
    long status = 0;
    check(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status));
    if (status != 200)
        throw std::runtime_error("Steam debugger HTTP request failed");
    auto targets = parse(body);
    return websocket_target(targets.get());
}
Json evaluation_result(json_object *message, int expected) {
    auto id = member(message, "id");
    if (!id || !json_object_is_type(id, json_type_int) || json_object_get_int(id) != expected)
        return own(nullptr); // CDP notifications and unrelated responses are not confirmations.
    auto result = member(message, "result");
    if (member(message, "error") || member(result, "exceptionDetails")) {
        const auto detail = member(result, "exceptionDetails");
        const auto description = string(member(member(detail, "exception"), "description"));
        throw std::runtime_error("Steam rejected the input filter: " +
                                 description.substr(0, description.find('\n')).substr(0, 200));
    }
    auto value = member(member(result, "result"), "value");
    if (!value || !json_object_is_type(value, json_type_object))
        throw std::runtime_error("Steam input filter returned no state");
    return own(json_object_get(value));
}
Json inactive_state(const std::string &serial) {
    auto state = own(json_object_new_object());
    json_object_object_add(state.get(), "active", json_object_new_boolean(false));
    json_object_object_add(state.get(), "index", json_object_new_int(-1));
    add(state.get(), "serial", serial);
    return state;
}
class Debugger final : public SteamUiConnection {
    boost::asio::io_context io;
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket, false> socket{io};
    int sequence = 0;
    template <class Start> void perform(double deadline, Start start) {
        const auto remaining = deadline - monotonic();
        if (remaining <= 0)
            throw std::runtime_error("Steam debugger response timed out");
        std::optional<boost::system::error_code> result;
        io.restart();
        start([&](boost::system::error_code error, auto...) { result = error; });
        io.run_for(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(remaining)));
        if (!result) {
            boost::system::error_code ignored;
            socket.next_layer().cancel(ignored);
            socket.next_layer().close(ignored);
            io.restart();
            io.run();
            throw std::runtime_error("Steam debugger response timed out");
        }
        if (*result)
            throw std::runtime_error("Steam debugger transport: " + result->message());
    }

  public:
    Debugger() {
        const auto url = discover();
        const double deadline = monotonic() + .5;
        const boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address_v4::loopback(),
                                                      8080);
        perform(deadline, [&](auto completion) {
            socket.next_layer().async_connect(endpoint, std::move(completion));
        });
        socket.next_layer().set_option(boost::asio::ip::tcp::no_delay(true));
        perform(deadline, [&](auto completion) {
            socket.async_handshake("127.0.0.1:8080",
                                   url.substr(std::string("ws://127.0.0.1:8080").size()),
                                   std::move(completion));
        });
        socket.read_message_max(max_message);
        socket.text(true);
    }
    Json renew(const SteamUiRequest &request) override {
        if (!serial_valid(request.serial) || !serial_valid(request.owner) ||
            (request.enabled && request.typing) || request.touches < 0 || request.touches > 3 ||
            (request.action_only && (!request.keyboard || request.enabled)) ||
            (request.system_only && (request.keyboard || request.action_only || request.typing)))
            throw std::invalid_argument("Invalid controller input lease request");
        return renew(request.serial, request.owner, request.enabled, request.keyboard,
                     request.action_only, request.typing, request.touches, request.held,
                     request.system_only, request.tap);
    }
    Json renew(const std::string &serial, const std::string &owner, bool enabled,
               bool keyboard = false, bool action_only = false, bool typing = false,
               int touches = 0, bool held = false, bool system_only = false, uint32_t tap = 0) {
        auto request = own(json_object_new_object());
        add(request.get(), "serial", serial);
        add(request.get(), "owner", owner);
        json_object_object_add(request.get(), "enabled", json_object_new_boolean(enabled));
        json_object_object_add(request.get(), "keyboard", json_object_new_boolean(keyboard));
        json_object_object_add(request.get(), "actionOnly", json_object_new_boolean(action_only));
        json_object_object_add(request.get(), "typing", json_object_new_boolean(typing));
        json_object_object_add(request.get(), "touches", json_object_new_int(touches));
        json_object_object_add(request.get(), "held", json_object_new_boolean(held));
        if (system_only) {
            json_object_object_add(request.get(), "systemOnly", json_object_new_boolean(true));
            json_object_object_add(request.get(), "tap", json_object_new_uint64(tap));
        }
        const std::string expression =
            std::string(input_filter_source) + "(" + encode(request.get()) + ")";
        auto params = own(json_object_new_object());
        add(params.get(), "expression", expression);
        json_object_object_add(params.get(), "returnByValue", json_object_new_boolean(true));
        json_object_object_add(params.get(), "timeout", json_object_new_int(200));
        auto command = own(json_object_new_object());
        const int id = ++sequence;
        json_object_object_add(command.get(), "id", json_object_new_int(id));
        add(command.get(), "method", "Runtime.evaluate");
        json_object_object_add(command.get(), "params", params.release());
        const std::string payload = encode(command.get());
        const double deadline = monotonic() + .25;
        perform(deadline, [&](auto completion) {
            socket.async_write(boost::asio::buffer(payload), std::move(completion));
        });
        boost::beast::flat_buffer message(max_message);
        while (monotonic() < deadline) {
            perform(deadline,
                    [&](auto completion) { socket.async_read(message, std::move(completion)); });
            if (!socket.got_text())
                throw std::runtime_error("Invalid Steam debugger websocket message");
            auto reply = parse(boost::beast::buffers_to_string(message.data()));
            message.consume(message.size());
            auto value = evaluation_result(reply.get(), id);
            if (value)
                return value;
        }
        throw std::runtime_error("Steam debugger response timed out");
    }
};
} // namespace ct::steamui

namespace ct {
std::unique_ptr<SteamUiConnection> connect_steam_ui() {
    return std::make_unique<steamui::Debugger>();
}
} // namespace ct
