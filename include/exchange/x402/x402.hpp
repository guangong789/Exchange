#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace exchange {
    struct ExternalPaymentRequirement {
        std::uint32_t x402_version{};
        std::string resource_url;
        std::string resource_description;
        std::string resource_mime_type;
        std::string scheme;
        std::string network;
        std::string amount;
        std::string asset;
        std::string pay_to;
        std::uint32_t max_timeout_seconds{};

        bool operator==(const ExternalPaymentRequirement&) const = default;
    };

    struct X402RequestResult {
        int http_status{};
        std::optional<ExternalPaymentRequirement> payment_required;
        std::optional<std::string> resource_body;

        bool operator==(const X402RequestResult&) const = default;
    };

    class X402Error : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class X402TransportError final : public X402Error {
    public:
        using X402Error::X402Error;
    };

    class X402ProtocolError final : public X402Error {
    public:
        using X402Error::X402Error;
    };

    class X402Client {
    public:
        virtual ~X402Client() = default;

        [[nodiscard]] virtual X402RequestResult get(
            std::string_view url) = 0;
    };

    struct CurlX402ClientConfig {
        std::chrono::milliseconds timeout{5'000};
    };

    class CurlX402Client final : public X402Client {
    public:
        explicit CurlX402Client(CurlX402ClientConfig config = {});

        CurlX402Client(const CurlX402Client&) = delete;
        CurlX402Client& operator=(const CurlX402Client&) = delete;
        CurlX402Client(CurlX402Client&&) = delete;
        CurlX402Client& operator=(CurlX402Client&&) = delete;

        [[nodiscard]] X402RequestResult get(
            std::string_view url) override;

    private:
        CurlX402ClientConfig config_;
    };

    struct X402PaidServiceConfig {
        std::string resource_path{"/premium-signal"};
        std::string description{"Premium market signal from Agent B"};
        std::string mime_type{"application/json"};
        std::string scheme{"exact"};
        std::string network{"eip155:97"};
        std::string asset{
            "0x330949Aed7d00FCe0558C64ED6FeC9792616cC39"};
        std::string amount{"10000"};
        std::string pay_to{
            "0x1111111111111111111111111111111111111111"};
        std::uint32_t max_timeout_seconds{60};
    };

    class X402PaidService {
    public:
        explicit X402PaidService(
            X402PaidServiceConfig config = {},
            std::uint16_t port = 0);
        ~X402PaidService() noexcept;

        X402PaidService(const X402PaidService&) = delete;
        X402PaidService& operator=(const X402PaidService&) = delete;
        X402PaidService(X402PaidService&&) = delete;
        X402PaidService& operator=(X402PaidService&&) = delete;

        [[nodiscard]] std::uint16_t local_port() const noexcept;
        [[nodiscard]] std::string resource_url() const;

        // Handles exactly one connection, then returns. The caller controls
        // threading and lifecycle for this localhost-only demo service.
        void serve_once();

    private:
        X402PaidServiceConfig config_;
        int listen_fd_{-1};
        std::uint16_t local_port_{};
    };
}  // namespace exchange
