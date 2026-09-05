#include "exchange/hackathon/live_x402_evidence.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace exchange {
    namespace {
        std::int64_t unix_ms() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
        void finish(LiveX402Evidence& value, std::int64_t started) {
            value.completed_at_unix_ms = unix_ms();
            value.duration_ms = value.completed_at_unix_ms - started;
        }
    }

    LiveX402EvidenceRunner::LiveX402EvidenceRunner(LiveX402EvidenceConfig config)
        : config_(std::move(config)) {}

    LiveX402RequirementEvidence LiveX402EvidenceRunner::known_requirement() {
        return {{2, "http://127.0.0.1/premium-signal",
                 "Exchange preview-only live x402 evidence", "application/json",
                 "exact", "eip155:56", "10000",
                 "0x55d398326f99059fF775485246999027B3197955",
                 "0x1111111111111111111111111111111111111111", 60},
                {"Tether USD", "1"}};
    }

    LiveX402Evidence LiveX402EvidenceRunner::run() const {
        LiveX402Evidence evidence;
        evidence.requirement = known_requirement();
        evidence.started_at_unix_ms = unix_ms();
        try {
            BinanceAgenticWalletCliBridge wallet({config_.baw_path, config_.timeout,
                evidence.requirement.accept_extra});
            const auto result = wallet.preview_with_evidence(evidence.requirement.requirement);
            evidence.wallet_status = result.wallet_status;
            evidence.provider = result.preview.provider;
            evidence.provider_status = result.preview.provider_status;
            evidence.reasons = result.preview.reasons;
            evidence.preview_performed = true;
            evidence.payment_signable = result.preview.status
                == ExternalPaymentPreviewStatus::Approved;
            evidence.status = LiveX402EvidenceStatus::Complete;
        } catch (const BinanceAgenticWalletNoSessionError&) {
            evidence.wallet_status = "DISCONNECTED";
            evidence.error_code = "WALLET_DISCONNECTED";
            evidence.error_message = "Binance Agentic Wallet 未连接。";
            evidence.status = LiveX402EvidenceStatus::Error;
        } catch (const BinanceAgenticWalletAuthenticationError&) {
            evidence.error_code = "WALLET_AUTHENTICATION_ERROR";
            evidence.error_message = "Binance Agentic Wallet 会话不可用。";
            evidence.status = LiveX402EvidenceStatus::Error;
        } catch (const BinanceAgenticWalletTransportError& error) {
            const std::string message = error.what();
            evidence.error_code = message.find("timed out") != std::string::npos
                ? "TIMEOUT" : message.find("failed to start baw") != std::string::npos
                ? "TOOL_UNAVAILABLE" : "TRANSPORT_ERROR";
            evidence.error_message = "Binance Agentic Wallet 预览调用未完成。";
            evidence.status = LiveX402EvidenceStatus::Error;
        } catch (const BinanceAgenticWalletResponseError&) {
            evidence.error_code = "INVALID_PROVIDER_RESPONSE";
            evidence.error_message = "Provider 响应无法安全解析。";
            evidence.status = LiveX402EvidenceStatus::Error;
        } catch (const BinanceAgenticWalletProviderError&) {
            evidence.error_code = "PROVIDER_ERROR";
            evidence.error_message = "Provider 拒绝了预览请求。";
            evidence.status = LiveX402EvidenceStatus::Error;
        } catch (const std::exception&) {
            evidence.error_code = "INTERNAL_ERROR";
            evidence.error_message = "Live x402 预览未能完成。";
            evidence.status = LiveX402EvidenceStatus::Error;
        }
        finish(evidence, evidence.started_at_unix_ms);
        return evidence;
    }
}  // namespace exchange
