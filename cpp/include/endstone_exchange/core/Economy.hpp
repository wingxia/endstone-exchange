#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace exchange {

class ExchangeException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class InsufficientFunds : public ExchangeException {
public:
    using ExchangeException::ExchangeException;
};

class InvalidOrder : public ExchangeException {
public:
    using ExchangeException::ExchangeException;
};

class Economy {
public:
    virtual ~Economy() = default;
    virtual std::int64_t balance(const std::string& player_name) = 0;
    virtual void debit(const std::string& player_name, std::int64_t amount, const std::string& idempotency_key) = 0;
    virtual void credit(const std::string& player_name, std::int64_t amount, const std::string& idempotency_key) = 0;
};

class FakeEconomy final : public Economy {
public:
    explicit FakeEconomy(std::unordered_map<std::string, std::int64_t> balances = {}) : balances_(std::move(balances)) {}

    std::int64_t balance(const std::string& player_name) override {
        return balances_[player_name];
    }

    void debit(const std::string& player_name, std::int64_t amount, const std::string&) override {
        if (amount <= 0) {
            return;
        }
        if (balances_[player_name] < amount) {
            throw InsufficientFunds("insufficient UMoney balance");
        }
        balances_[player_name] -= amount;
    }

    void credit(const std::string& player_name, std::int64_t amount, const std::string&) override {
        if (amount <= 0) {
            return;
        }
        balances_[player_name] += amount;
    }

    std::unordered_map<std::string, std::int64_t> balances_;
};

class HttpBridgeEconomy final : public Economy {
public:
    HttpBridgeEconomy(std::string host, int port, std::string token);

    std::int64_t balance(const std::string& player_name) override;
    void debit(const std::string& player_name, std::int64_t amount, const std::string& idempotency_key) override;
    void credit(const std::string& player_name, std::int64_t amount, const std::string& idempotency_key) override;

private:
    std::string request(const std::string& method, const std::string& path, const std::string& body = "");
    static std::int64_t parseBalance(const std::string& json);
    static std::string escapeJson(const std::string& value);
    static std::string urlEncode(const std::string& value);

    std::string host_;
    int port_;
    std::string token_;
};

}  // namespace exchange
