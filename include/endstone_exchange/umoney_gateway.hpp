#pragma once

#include "endstone_exchange/config.hpp"
#include "endstone_exchange/domain.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace exchange {

struct UmoneyTransferRequest {
    Id transfer_id{0};
    std::string operation_key;
    std::string player_name;
    EconomyTransferDirection direction{EconomyTransferDirection::Deposit};
    std::int64_t amount_units{0};
};

struct UmoneyTransferResult {
    Id transfer_id{0};
    std::string player_name;
    EconomyTransferDirection direction{EconomyTransferDirection::Deposit};
    std::int64_t amount_units{0};
    bool success{false};
    bool retryable{true};
    bool applied{false};
    std::int64_t external_balance_units{0};
    std::string error_code;
    std::string error;
};

class UmoneyTransferWorker {
  public:
    explicit UmoneyTransferWorker(EconomyConfig config);
    ~UmoneyTransferWorker();

    UmoneyTransferWorker(const UmoneyTransferWorker &) = delete;
    UmoneyTransferWorker &operator=(const UmoneyTransferWorker &) = delete;

    bool submit(UmoneyTransferRequest request);
    [[nodiscard]] std::vector<UmoneyTransferResult> takeResults();
    [[nodiscard]] std::size_t outstandingCount() const;
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace exchange
