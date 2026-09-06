#include "exchange/hackathon/live_x402_evidence.hpp"
#include "exchange/accounting/account_store.hpp"
#include "exchange/accounting/ledger.hpp"
#include "exchange/matching/order_book.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        class TemporaryExecutable {
        public:
            explicit TemporaryExecutable(std::string contents) {
                std::string pattern = "/tmp/exchange-live-x402-XXXXXX";
                const int fd = ::mkstemp(pattern.data());
                if (fd < 0) throw std::runtime_error("mkstemp failed");
                static_cast<void>(::close(fd));
                path_ = pattern;
                std::ofstream output(path_);
                output << contents;
                output.close();
                if (!output || ::chmod(path_.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
                    throw std::runtime_error("failed to create executable");
                }
            }
            ~TemporaryExecutable() { std::error_code ignored; std::filesystem::remove(path_, ignored); }
            const std::string& path() const noexcept { return path_; }
        private:
            std::string path_;
        };

        TEST(LiveX402EvidenceTest, PreservesKnownBinanceRequirementMetadata) {
            const auto requirement = LiveX402EvidenceRunner::known_requirement();
            EXPECT_EQ(requirement.requirement.x402_version, 2U);
            EXPECT_EQ(requirement.requirement.network, "eip155:56");
            EXPECT_EQ(requirement.requirement.amount, "10000");
            EXPECT_EQ(requirement.accept_extra.name, "Tether USD");
            EXPECT_EQ(requirement.accept_extra.version, "1");
        }

        TEST(LiveX402EvidenceTest, MapsRealPreviewBoundaryWithoutSettlement) {
            TemporaryExecutable baw("#!/bin/sh\n"
                "if [ \"$1\" = wallet ]; then printf '%s' '{\"success\":true,\"data\":{\"status\":\"CONNECTED\"}}'; exit 0; fi\n"
                "printf '%s' '{\"success\":true,\"data\":{\"paymentId\":\"id\",\"options\":[{\"status\":\"ACTION_REQUIRED\",\"reasons\":[\"INSUFFICIENT_BALANCE\"],\"originalAccept\":{\"scheme\":\"exact\",\"network\":\"eip155:56\",\"asset\":\"0x55d398326f99059fF775485246999027B3197955\",\"amount\":\"10000\",\"payTo\":\"0x1111111111111111111111111111111111111111\"}}]}}'\n");
            const LiveX402Evidence evidence = LiveX402EvidenceRunner(
                {baw.path(), std::chrono::seconds(2)}).run();
            EXPECT_EQ(evidence.status, LiveX402EvidenceStatus::Complete);
            EXPECT_EQ(evidence.wallet_status, "CONNECTED");
            EXPECT_EQ(evidence.provider_status, "ACTION_REQUIRED");
            EXPECT_EQ(evidence.reasons, (std::vector<std::string>{"INSUFFICIENT_BALANCE"}));
            EXPECT_TRUE(evidence.preview_performed);
            EXPECT_FALSE(evidence.payment_signable);
            EXPECT_FALSE(evidence.payment_signed);
            EXPECT_FALSE(evidence.broadcast);
            EXPECT_FALSE(evidence.settlement_performed);
            EXPECT_FALSE(evidence.service_unlocked);
        }

        TEST(LiveX402EvidenceTest, MapsMissingCliToSafeToolUnavailableError) {
            const LiveX402Evidence evidence = LiveX402EvidenceRunner(
                {"/definitely/missing/baw", std::chrono::milliseconds(20)}).run();
            EXPECT_EQ(evidence.status, LiveX402EvidenceStatus::Error);
            EXPECT_EQ(evidence.error_code, "TOOL_UNAVAILABLE");
            EXPECT_TRUE(evidence.provider.empty());
            EXPECT_TRUE(evidence.reasons.empty());
            EXPECT_FALSE(evidence.preview_performed);
        }

        TEST(LiveX402EvidenceTest, DisconnectedWalletStopsBeforePreview) {
            TemporaryExecutable baw("#!/bin/sh\n"
                "printf '%s' '{\"success\":true,\"data\":{\"status\":\"DISCONNECTED\"}}'\n");
            const LiveX402Evidence evidence = LiveX402EvidenceRunner(
                {baw.path(), std::chrono::seconds(2)}).run();
            EXPECT_EQ(evidence.status, LiveX402EvidenceStatus::Error);
            EXPECT_EQ(evidence.wallet_status, "DISCONNECTED");
            EXPECT_EQ(evidence.error_code, "WALLET_DISCONNECTED");
            EXPECT_FALSE(evidence.preview_performed);
        }

        TEST(LiveX402EvidenceTest, MapsPreviewTimeoutToSafeError) {
            TemporaryExecutable baw("#!/bin/sh\n"
                "if [ \"$1\" = wallet ]; then printf '%s' '{\"success\":true,\"data\":{\"status\":\"CONNECTED\"}}'; exit 0; fi\n"
                "sleep 1\n");
            const LiveX402Evidence evidence = LiveX402EvidenceRunner(
                {baw.path(), std::chrono::milliseconds(20)}).run();
            EXPECT_EQ(evidence.status, LiveX402EvidenceStatus::Error);
            EXPECT_EQ(evidence.error_code, "TIMEOUT");
            EXPECT_FALSE(evidence.preview_performed);
        }

        TEST(LiveX402EvidenceTest, ProviderErrorsAreRedactedAndDoNotTouchLocalCore) {
            AccountStore accounts;
            ASSERT_TRUE(accounts.create_account(7));
            accounts.fund(7, 1, 100);
            Ledger ledger;
            OrderBook order_book;
            TemporaryExecutable baw("#!/bin/sh\n"
                "if [ \"$1\" = wallet ]; then printf '%s' '{\"success\":true,\"data\":{\"status\":\"CONNECTED\"}}'; exit 0; fi\n"
                "printf '%s' '{\"success\":false,\"error\":{\"name\":\"SECRET_TOKEN_SHOULD_NOT_LEAK\"}}'\n");
            const LiveX402Evidence evidence = LiveX402EvidenceRunner(
                {baw.path(), std::chrono::seconds(2)}).run();
            EXPECT_EQ(evidence.status, LiveX402EvidenceStatus::Error);
            EXPECT_EQ(evidence.error_code, "PROVIDER_ERROR");
            EXPECT_EQ(evidence.error_message.find("SECRET_TOKEN"), std::string::npos);
            EXPECT_EQ(accounts.find_balance(7, 1), (Balance{100, 0}));
            EXPECT_TRUE(ledger.entries().empty());
            EXPECT_EQ(order_book.order_count(), 0U);
        }
    }
}  // namespace exchange
