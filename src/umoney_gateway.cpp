#include "endstone_exchange/umoney_gateway.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace exchange {
namespace {

class BridgeError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

std::string jsonEscape(const std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char c : value) {
        switch (c) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (c < 0x20) {
                result += std::format("\\u{:04x}", c);
            } else {
                result.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return result;
}

std::optional<std::size_t> jsonValueStart(const std::string_view json, const std::string_view key) {
    const auto marker = "\"" + std::string(key) + "\"";
    const auto key_position = json.find(marker);
    if (key_position == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = json.find(':', key_position + marker.size());
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto position = colon + 1;
    while (position < json.size() && (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
                                      json[position] == '\n')) {
        ++position;
    }
    return position;
}

std::optional<bool> jsonBool(const std::string_view json, const std::string_view key) {
    const auto start = jsonValueStart(json, key);
    if (!start) {
        return std::nullopt;
    }
    if (json.substr(*start, 4) == "true") {
        return true;
    }
    if (json.substr(*start, 5) == "false") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::int64_t> jsonInt64(const std::string_view json, const std::string_view key) {
    const auto start = jsonValueStart(json, key);
    if (!start) {
        return std::nullopt;
    }
    auto end = *start;
    if (end < json.size() && (json[end] == '-' || json[end] == '+')) {
        ++end;
    }
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') {
        ++end;
    }
    if (end == *start || (end == *start + 1 && (json[*start] == '-' || json[*start] == '+'))) {
        return std::nullopt;
    }
    std::int64_t value{};
    const auto [parsed_end, error] = std::from_chars(json.data() + *start, json.data() + end, value);
    if (error != std::errc{} || parsed_end != json.data() + end) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> jsonString(const std::string_view json, const std::string_view key) {
    const auto start = jsonValueStart(json, key);
    if (!start || *start >= json.size() || json[*start] != '"') {
        return std::nullopt;
    }
    std::string result;
    bool escaped = false;
    for (auto position = *start + 1; position < json.size(); ++position) {
        const char c = json[position];
        if (escaped) {
            switch (c) {
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            default:
                result.push_back(c);
                break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return result;
        } else {
            result.push_back(c);
        }
    }
    return std::nullopt;
}

struct HttpResponse {
    int status{0};
    std::string body;
};

#if !defined(_WIN32)
class SocketHandle {
  public:
    explicit SocketHandle(const int fd) : fd_(fd) {}
    ~SocketHandle() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    SocketHandle(const SocketHandle &) = delete;
    SocketHandle &operator=(const SocketHandle &) = delete;
    [[nodiscard]] int get() const noexcept { return fd_; }

  private:
    int fd_;
};

int connectSocket(const std::string &host, const unsigned int port, const unsigned int timeout_milliseconds) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    const auto port_text = std::to_string(port);
    const auto resolve_result = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses);
    if (resolve_result != 0) {
        throw BridgeError("could not resolve the UMoney bridge host");
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address_guard(addresses, freeaddrinfo);

    for (auto *address = addresses; address != nullptr; address = address->ai_next) {
        const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        const int original_flags = ::fcntl(fd, F_GETFL, 0);
        if (original_flags < 0 || ::fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
            ::close(fd);
            continue;
        }

        bool connected = ::connect(fd, address->ai_addr, address->ai_addrlen) == 0;
        if (!connected && errno == EINPROGRESS) {
            pollfd descriptor{fd, POLLOUT, 0};
            const auto poll_result = ::poll(&descriptor, 1, static_cast<int>(timeout_milliseconds));
            if (poll_result > 0 && (descriptor.revents & POLLOUT) != 0) {
                int socket_error = 0;
                socklen_t error_length = sizeof(socket_error);
                connected = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) == 0 &&
                            socket_error == 0;
            }
        }
        if (!connected) {
            ::close(fd);
            continue;
        }
        if (::fcntl(fd, F_SETFL, original_flags) < 0) {
            ::close(fd);
            continue;
        }
        timeval timeout{static_cast<time_t>(timeout_milliseconds / 1000),
                        static_cast<suseconds_t>((timeout_milliseconds % 1000) * 1000)};
        static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
        static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
        return fd;
    }
    throw BridgeError("could not connect to the UMoney bridge");
}

void sendAll(const int fd, const std::string_view data) {
    std::size_t position = 0;
    while (position < data.size()) {
#if defined(MSG_NOSIGNAL)
        const auto sent = ::send(fd, data.data() + position, data.size() - position, MSG_NOSIGNAL);
#else
        const auto sent = ::send(fd, data.data() + position, data.size() - position, 0);
#endif
        if (sent <= 0) {
            throw BridgeError("could not send a request to the UMoney bridge");
        }
        position += static_cast<std::size_t>(sent);
    }
}
#endif

HttpResponse httpPost(const EconomyConfig &config, const std::string_view path, const std::string_view body) {
#if defined(_WIN32)
    static_cast<void>(config);
    static_cast<void>(path);
    static_cast<void>(body);
    throw BridgeError("the UMoney bridge client is supported only by the Linux plugin build");
#else
    SocketHandle socket(connectSocket(config.bridge_host, config.bridge_port, config.request_timeout_milliseconds));
    const auto request =
        std::format("POST {} HTTP/1.1\r\nHost: {}:{}\r\nAuthorization: Bearer {}\r\n"
                    "Content-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                    path, config.bridge_host, config.bridge_port, config.bridge_token, body.size(), body);
    sendAll(socket.get(), request);

    std::string response;
    std::array<char, 4096> buffer{};
    while (response.size() <= 1024 * 1024) {
        const auto received = ::recv(socket.get(), buffer.data(), buffer.size(), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            throw BridgeError("could not receive a response from the UMoney bridge");
        }
        response.append(buffer.data(), static_cast<std::size_t>(received));
    }
    if (response.size() > 1024 * 1024) {
        throw BridgeError("UMoney bridge response is too large");
    }
    const auto line_end = response.find("\r\n");
    const auto body_start = response.find("\r\n\r\n");
    if (line_end == std::string::npos || body_start == std::string::npos) {
        throw BridgeError("UMoney bridge returned a malformed HTTP response");
    }
    const auto first_space = response.find(' ');
    if (first_space == std::string::npos || first_space >= line_end) {
        throw BridgeError("UMoney bridge returned a malformed status line");
    }
    const auto second_space = response.find(' ', first_space + 1);
    const auto status_end = second_space == std::string::npos || second_space > line_end ? line_end : second_space;
    int status{};
    const auto [parsed_end, error] =
        std::from_chars(response.data() + first_space + 1, response.data() + status_end, status);
    if (error != std::errc{} || parsed_end != response.data() + status_end) {
        throw BridgeError("UMoney bridge returned a malformed status code");
    }
    return {status, response.substr(body_start + 4)};
#endif
}

UmoneyTransferResult applyTransfer(const EconomyConfig &config, const UmoneyTransferRequest &request) {
    UmoneyTransferResult result;
    result.transfer_id = request.transfer_id;
    result.player_name = request.player_name;
    result.direction = request.direction;
    result.amount_units = request.amount_units;
    const auto path = request.direction == EconomyTransferDirection::Deposit ? "/debit" : "/credit";
    const auto body =
        std::format(R"({{"player":"{}","amount":{},"idempotency_key":"{}"}})",
                    jsonEscape(request.player_name), request.amount_units, jsonEscape(request.operation_key));

    try {
        const auto response = httpPost(config, path, body);
        const auto ok = jsonBool(response.body, "ok").value_or(false);
        if (response.status == 200 && ok) {
            const auto balance = jsonInt64(response.body, "balance");
            if (!balance) {
                throw BridgeError("UMoney bridge response did not contain a balance");
            }
            result.success = true;
            result.retryable = false;
            result.applied = true;
            result.external_balance_units = *balance;
            return result;
        }
        result.retryable = jsonBool(response.body, "retryable").value_or(response.status >= 500);
        result.applied = jsonBool(response.body, "applied").value_or(false);
        result.error_code = jsonString(response.body, "code").value_or("bridge_error");
        result.error = jsonString(response.body, "error").value_or(std::format("UMoney bridge HTTP {}", response.status));
        if (result.applied) {
            result.retryable = true;
        }
    } catch (const std::exception &error) {
        result.retryable = true;
        result.error_code = "bridge_unavailable";
        result.error = error.what();
    }
    return result;
}

} // namespace

struct UmoneyTransferWorker::Impl {
    explicit Impl(EconomyConfig bridge_config) : config(std::move(bridge_config)) {
        worker = std::jthread([this](const std::stop_token token) { run(token); });
    }

    ~Impl() { stop(); }

    void run(const std::stop_token token) {
        while (!token.stop_requested()) {
            UmoneyTransferRequest request;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return token.stop_requested() || !requests.empty(); });
                if (token.stop_requested()) {
                    break;
                }
                request = std::move(requests.front());
                requests.pop_front();
            }
            auto result = applyTransfer(config, request);
            {
                std::lock_guard lock(mutex);
                results.push_back(std::move(result));
            }
        }
    }

    void stop() {
        if (worker.joinable()) {
            worker.request_stop();
            condition.notify_all();
            worker.join();
        }
    }

    EconomyConfig config;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<UmoneyTransferRequest> requests;
    std::deque<UmoneyTransferResult> results;
    std::unordered_set<Id> outstanding;
    std::jthread worker;
};

UmoneyTransferWorker::UmoneyTransferWorker(EconomyConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

UmoneyTransferWorker::~UmoneyTransferWorker() = default;

bool UmoneyTransferWorker::submit(UmoneyTransferRequest request) {
    if (request.transfer_id == 0 || request.operation_key.empty() || request.player_name.empty() ||
        request.amount_units <= 0) {
        throw std::runtime_error("UMoney transfer request is incomplete");
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->outstanding.insert(request.transfer_id).second) {
        return false;
    }
    impl_->requests.push_back(std::move(request));
    impl_->condition.notify_one();
    return true;
}

std::vector<UmoneyTransferResult> UmoneyTransferWorker::takeResults() {
    std::lock_guard lock(impl_->mutex);
    std::vector<UmoneyTransferResult> result;
    result.reserve(impl_->results.size());
    while (!impl_->results.empty()) {
        impl_->outstanding.erase(impl_->results.front().transfer_id);
        result.push_back(std::move(impl_->results.front()));
        impl_->results.pop_front();
    }
    return result;
}

std::size_t UmoneyTransferWorker::outstandingCount() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->outstanding.size();
}

void UmoneyTransferWorker::stop() {
    if (impl_) {
        impl_->stop();
    }
}

} // namespace exchange
