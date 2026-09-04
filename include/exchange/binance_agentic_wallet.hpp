#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

#include "exchange/external_payment_preview.hpp"
#include "exchange/x402.hpp"

namespace exchange {
    class BinanceAgenticWalletError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class BinanceAgenticWalletNoSessionError final
        : public BinanceAgenticWalletError {
    public:
        using BinanceAgenticWalletError::BinanceAgenticWalletError;
    };

    class BinanceAgenticWalletAuthenticationError final
        : public BinanceAgenticWalletError {
    public:
        using BinanceAgenticWalletError::BinanceAgenticWalletError;
    };

    class BinanceAgenticWalletTransportError final
        : public BinanceAgenticWalletError {
    public:
        using BinanceAgenticWalletError::BinanceAgenticWalletError;
    };

    class BinanceAgenticWalletResponseError final
        : public BinanceAgenticWalletError {
    public:
        using BinanceAgenticWalletError::BinanceAgenticWalletError;
    };

    class BinanceAgenticWalletProviderError final
        : public BinanceAgenticWalletError {
    public:
        using BinanceAgenticWalletError::BinanceAgenticWalletError;
    };

    class BinanceAgenticWalletBridge {
    public:
        virtual ~BinanceAgenticWalletBridge() = default;

        [[nodiscard]] virtual ExternalPaymentPreview preview(
            const ExternalPaymentRequirement& requirement) = 0;
    };

    struct BinanceX402AcceptExtra {
        std::string name;
        std::string version;
    };

    struct BinanceAgenticWalletCliConfig {
        std::string baw_path{"baw"};
        std::chrono::milliseconds timeout{30'000};
        std::optional<BinanceX402AcceptExtra> accept_extra;
    };

    class BinanceAgenticWalletCliBridge final
        : public BinanceAgenticWalletBridge {
    public:
        explicit BinanceAgenticWalletCliBridge(
            BinanceAgenticWalletCliConfig config = {});

        BinanceAgenticWalletCliBridge(
            const BinanceAgenticWalletCliBridge&) = delete;
        BinanceAgenticWalletCliBridge& operator=(
            const BinanceAgenticWalletCliBridge&) = delete;
        BinanceAgenticWalletCliBridge(
            BinanceAgenticWalletCliBridge&&) = delete;
        BinanceAgenticWalletCliBridge& operator=(
            BinanceAgenticWalletCliBridge&&) = delete;

        [[nodiscard]] ExternalPaymentPreview preview(
            const ExternalPaymentRequirement& requirement) override;

    private:
        BinanceAgenticWalletCliConfig config_;
    };

    // Demo-only access gate. Approval means only that a wallet preview was
    // READY_TO_SIGN; it does not mean payment, signing, or settlement occurred.
    class PreviewAuthorizedAccessGate {
    public:
        explicit PreviewAuthorizedAccessGate(std::string protected_output);

        [[nodiscard]] bool apply_preview(
            const ExternalPaymentPreview& preview) noexcept;
        [[nodiscard]] bool preview_authorized() const noexcept;
        [[nodiscard]] std::optional<std::string> access() const;

    private:
        std::string protected_output_;
        bool preview_authorized_{};
    };
}  // namespace exchange
