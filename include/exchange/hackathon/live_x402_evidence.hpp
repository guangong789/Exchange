#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "exchange/binance/binance_agentic_wallet.hpp"

namespace exchange {
    enum class LiveX402EvidenceStatus { NotRun, Complete, Error };

    struct LiveX402RequirementEvidence {
        ExternalPaymentRequirement requirement;
        BinanceX402AcceptExtra accept_extra{"Tether USD", "1"};
        bool operator==(const LiveX402RequirementEvidence&) const = default;
    };

    struct LiveX402Evidence {
        LiveX402EvidenceStatus status{LiveX402EvidenceStatus::NotRun};
        LiveX402RequirementEvidence requirement;
        std::string wallet_status;
        std::string provider;
        std::string provider_status;
        std::vector<std::string> reasons;
        bool preview_performed{};
        bool payment_signable{};
        bool payment_signed{};
        bool broadcast{};
        bool settlement_performed{};
        bool service_unlocked{};
        std::string error_code;
        std::string error_message;
        std::int64_t started_at_unix_ms{};
        std::int64_t completed_at_unix_ms{};
        std::int64_t duration_ms{};
        bool operator==(const LiveX402Evidence&) const = default;
    };

    struct LiveX402EvidenceConfig {
        std::string baw_path{"baw"};
        std::chrono::milliseconds timeout{30'000};
    };

    class LiveX402EvidenceRunner {
    public:
        explicit LiveX402EvidenceRunner(LiveX402EvidenceConfig config = {});
        [[nodiscard]] LiveX402Evidence run() const;
        [[nodiscard]] static LiveX402RequirementEvidence known_requirement();
    private:
        LiveX402EvidenceConfig config_;
    };
}  // namespace exchange
