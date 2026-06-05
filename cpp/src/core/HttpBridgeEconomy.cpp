#include "endstone_exchange/core/Economy.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace exchange {
namespace {

class SocketHandle {
public:
    explicit SocketHandle(int fd) : fd_(fd) {}
    ~SocketHandle() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    int fd() const { return fd_; }

private:
    int fd_;
};

void sendAll(int fd, const std::string& data) {
    const char* ptr = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const auto sent = ::send(fd, ptr, remaining, 0);
        if (sent <= 0) {
            throw ExchangeException("failed to send request to UMoney bridge");
        }
        ptr += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
}

}  // namespace

HttpBridgeEconomy::HttpBridgeEconomy(std::string host, int port, std::string token)
    : host_(std::move(host)), port_(port), token_(std::move(token)) {}

std::int64_t HttpBridgeEconomy::balance(const std::string& player_name) {
    return parseBalance(request("GET", "/balance?player=" + urlEncode(player_name)));
}

void HttpBridgeEconomy::debit(const std::string& player_name, std::int64_t amount, const std::string& idempotency_key) {
    if (amount <= 0) {
        return;
    }
    std::ostringstream body;
    body << "{\"player\":\"" << escapeJson(player_name) << "\",\"amount\":" << amount
         << ",\"idempotency_key\":\"" << escapeJson(idempotency_key) << "\"}";
    request("POST", "/debit", body.str());
}

void HttpBridgeEconomy::credit(const std::string& player_name, std::int64_t amount, const std::string& idempotency_key) {
    if (amount <= 0) {
        return;
    }
    std::ostringstream body;
    body << "{\"player\":\"" << escapeJson(player_name) << "\",\"amount\":" << amount
         << ",\"idempotency_key\":\"" << escapeJson(idempotency_key) << "\"}";
    request("POST", "/credit", body.str());
}

std::string HttpBridgeEconomy::request(const std::string& method, const std::string& path, const std::string& body) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const auto port = std::to_string(port_);
    if (::getaddrinfo(host_.c_str(), port.c_str(), &hints, &result) != 0) {
        throw ExchangeException("failed to resolve UMoney bridge host");
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(result, freeaddrinfo);

    int fd = -1;
    for (auto* rp = result; rp != nullptr; rp = rp->ai_next) {
        fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    if (fd < 0) {
        throw ExchangeException("failed to connect to UMoney bridge");
    }
    SocketHandle socket(fd);

    std::ostringstream req;
    req << method << ' ' << path << " HTTP/1.1\r\n"
        << "Host: " << host_ << ':' << port_ << "\r\n"
        << "Authorization: Bearer " << token_ << "\r\n"
        << "Connection: close\r\n";
    if (!body.empty()) {
        req << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n" << body;
    sendAll(socket.fd(), req.str());

    std::string response;
    char buffer[4096];
    while (true) {
        const auto received = ::recv(socket.fd(), buffer, sizeof(buffer), 0);
        if (received < 0) {
            throw ExchangeException("failed to receive response from UMoney bridge");
        }
        if (received == 0) {
            break;
        }
        response.append(buffer, buffer + received);
    }
    const auto line_end = response.find("\r\n");
    if (line_end == std::string::npos || response.substr(0, 12).find("200") == std::string::npos) {
        throw ExchangeException("UMoney bridge returned non-200 response: " + response.substr(0, std::min<std::size_t>(120, response.size())));
    }
    const auto body_pos = response.find("\r\n\r\n");
    if (body_pos == std::string::npos) {
        return {};
    }
    return response.substr(body_pos + 4);
}

std::int64_t HttpBridgeEconomy::parseBalance(const std::string& json) {
    const auto key = json.find("\"balance\"");
    if (key == std::string::npos) {
        throw ExchangeException("UMoney bridge response missing balance");
    }
    const auto colon = json.find(':', key);
    if (colon == std::string::npos) {
        throw ExchangeException("UMoney bridge balance is malformed");
    }
    auto end = colon + 1;
    while (end < json.size() && (json[end] == ' ' || json[end] == '\t')) {
        ++end;
    }
    auto start = end;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) {
        ++end;
    }
    return std::stoll(json.substr(start, end - start));
}

std::string HttpBridgeEconomy::escapeJson(const std::string& value) {
    std::string out;
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::string HttpBridgeEconomy::urlEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace exchange
