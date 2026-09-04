#include "exchange/binance_agentic_wallet.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "exchange/account_store.hpp"
#include "exchange/ledger.hpp"
#include "exchange/order_book.hpp"

namespace exchange {
    namespace {
        ExternalPaymentRequirement test_requirement() {
            return ExternalPaymentRequirement{
                2,
                "http://127.0.0.1:9000/premium-signal",
                "Premium market signal from Agent B",
                "application/json",
                "exact",
                "eip155:56",
                "10000",
                "0x55d398326f99059fF775485246999027B3197955",
                "0x1111111111111111111111111111111111111111",
                60,
            };
        }

        X402PaidServiceConfig localhost_config() {
            return X402PaidServiceConfig{
                "/premium-signal",
                "Premium market signal from Agent B",
                "application/json",
                "exact",
                "eip155:97",
                "0x330949Aed7d00FCe0558C64ED6FeC9792616cC39",
                "10000",
                "0x1111111111111111111111111111111111111111",
                60,
            };
        }

        class FakeBinanceAgenticWalletBridge final
            : public BinanceAgenticWalletBridge {
        public:
            explicit FakeBinanceAgenticWalletBridge(
                ExternalPaymentPreview preview)
                : preview_(std::move(preview)) {}

            ExternalPaymentPreview preview(
                const ExternalPaymentRequirement& requirement) override {
                ++call_count_;
                last_requirement_ = requirement;
                return preview_;
            }

            std::size_t call_count() const noexcept { return call_count_; }

            const std::optional<ExternalPaymentRequirement>&
            last_requirement() const noexcept {
                return last_requirement_;
            }

        private:
            ExternalPaymentPreview preview_;
            std::size_t call_count_{};
            std::optional<ExternalPaymentRequirement> last_requirement_;
        };

        class TemporaryExecutable {
        public:
            explicit TemporaryExecutable(std::string_view contents) {
                std::string pattern = "/tmp/exchange-baw-test-XXXXXX";
                const int fd = ::mkstemp(pattern.data());
                if (fd < 0) {
                    throw std::runtime_error("mkstemp failed");
                }
                static_cast<void>(::close(fd));
                path_ = pattern;

                std::ofstream output(path_);
                output << contents;
                output.close();
                if (!output || ::chmod(path_.c_str(), S_IRUSR | S_IWUSR | S_IXUSR)
                                  != 0) {
                    std::filesystem::remove(path_);
                    throw std::runtime_error("failed to create test executable");
                }
            }

            ~TemporaryExecutable() {
                std::error_code ignored;
                std::filesystem::remove(path_, ignored);
            }

            TemporaryExecutable(const TemporaryExecutable&) = delete;
            TemporaryExecutable& operator=(const TemporaryExecutable&) = delete;

            const std::string& path() const noexcept { return path_; }

        private:
            std::string path_;
        };

        std::string fake_cli(std::string_view preview_response) {
            return std::string{
                "#!/bin/sh\n"
                "if [ \"$1\" = wallet ] && [ \"$2\" = status ]; then\n"
                "  printf '%s' '{\"success\":true,\"data\":{\"status\":\"CONNECTED\"}}'\n"
                "  exit 0\n"
                "fi\n"
                "if [ \"$1\" = x402-payment ] && [ \"$2\" = preview ]; then\n"
                "  printf '%s' '"}
                + std::string(preview_response)
                + "'\n  exit 0\nfi\nexit 9\n";
        }

        std::string fake_cli_requiring_binance_accept_extra(
            std::string_view preview_response) {
            return std::string{
                "#!/bin/sh\n"
                "if [ \"$1\" = wallet ] && [ \"$2\" = status ]; then\n"
                "  printf '%s' '{\"success\":true,\"data\":{\"status\":\"CONNECTED\"}}'\n"
                "  exit 0\n"
                "fi\n"
                "if [ \"$1\" = x402-payment ] && [ \"$2\" = preview ]; then\n"
                "  case \"$4\" in\n"
                "    *'\"extra\":{\"name\":\"Tether USD\",\"version\":\"1\"}'*)\n"
                "      printf '%s' '"}
                + std::string(preview_response)
                + "'\n      exit 0\n      ;;\n"
                  "  esac\n"
                  "fi\n"
                  "exit 9\n";
        }

        constexpr std::string_view approved_response =
            R"({"success":true,"data":{"paymentId":"preview-only-id","options":[{"index":1,"status":"READY_TO_SIGN","reasons":[],"originalAccept":{"scheme":"exact","network":"eip155:56","asset":"0x55d398326f99059fF775485246999027B3197955","amount":"10000","payTo":"0x1111111111111111111111111111111111111111"}}]}})";

        constexpr std::string_view rejected_response =
            R"({"success":true,"data":{"paymentId":"preview-only-id","options":[{"index":1,"status":"ACTION_REQUIRED","reasons":["INSUFFICIENT_BALANCE"],"tokenSymbol":null,"amount":null,"originalAccept":{"scheme":"exact","network":"eip155:56","asset":"0x55d398326f99059fF775485246999027B3197955","amount":"10000","payTo":"0x1111111111111111111111111111111111111111"}}]}})";

        constexpr std::string_view not_signable_response =
            R"({"success":true,"data":{"paymentId":"preview-only-id","options":[{"index":1,"status":"NOT_SIGNABLE","reasons":["INVALID_ACCEPT_STRUCTURE"],"tokenSymbol":null,"amount":null,"binanceChainId":null,"assetTransferMethod":null,"originalAccept":{"scheme":"exact","network":"eip155:56","asset":"0x55d398326f99059fF775485246999027B3197955","amount":"10000","payTo":"0x1111111111111111111111111111111111111111","maxTimeoutSeconds":60}}]}})";

        TEST(BinanceAgenticWalletBridgeTest,
             NormalizesReadyToSignWithoutProducingPaymentSignature) {
            TemporaryExecutable baw(fake_cli(approved_response));
            BinanceAgenticWalletCliBridge bridge(
                {baw.path(), std::chrono::seconds(2)});

            EXPECT_EQ(
                bridge.preview(test_requirement()),
                (ExternalPaymentPreview{
                    ExternalPaymentPreviewStatus::Approved,
                    "binance-agentic-wallet",
                    "eip155:56",
                    "0x55d398326f99059fF775485246999027B3197955",
                    "10000",
                    "READY_TO_SIGN",
                    {}}));
        }

        TEST(BinanceAgenticWalletBridgeTest,
             AddsConfiguredBinanceAcceptExtraWithoutChangingRequirement) {
            TemporaryExecutable baw(
                fake_cli_requiring_binance_accept_extra(approved_response));
            BinanceAgenticWalletCliBridge bridge(
                {baw.path(),
                 std::chrono::seconds(2),
                 BinanceX402AcceptExtra{"Tether USD", "1"}});

            EXPECT_EQ(
                bridge.preview(test_requirement()).status,
                ExternalPaymentPreviewStatus::Approved);
        }

        TEST(BinanceAgenticWalletBridgeTest,
             NormalizesInsufficientBalanceAsRejectedPreview) {
            TemporaryExecutable baw(fake_cli(rejected_response));
            BinanceAgenticWalletCliBridge bridge(
                {baw.path(), std::chrono::seconds(2)});

            EXPECT_EQ(
                bridge.preview(test_requirement()),
                (ExternalPaymentPreview{
                    ExternalPaymentPreviewStatus::Rejected,
                    "binance-agentic-wallet",
                    "eip155:56",
                    "0x55d398326f99059fF775485246999027B3197955",
                    "10000",
                    "ACTION_REQUIRED",
                    {"INSUFFICIENT_BALANCE"}}));
        }

        TEST(BinanceAgenticWalletBridgeTest,
             NormalizesCurrentNotSignableShapeWithoutTokenMetadata) {
            TemporaryExecutable baw(fake_cli(not_signable_response));
            BinanceAgenticWalletCliBridge bridge(
                {baw.path(), std::chrono::seconds(2)});

            EXPECT_EQ(
                bridge.preview(test_requirement()),
                (ExternalPaymentPreview{
                    ExternalPaymentPreviewStatus::Rejected,
                    "binance-agentic-wallet",
                    "eip155:56",
                    "0x55d398326f99059fF775485246999027B3197955",
                    "10000",
                    "NOT_SIGNABLE",
                    {"INVALID_ACCEPT_STRUCTURE"}}));
        }

        TEST(BinanceAgenticWalletBridgeTest, RejectsMissingRequiredStatus) {
            constexpr std::string_view missing_status =
                R"({"success":true,"data":{"paymentId":"preview-only-id","options":[{"reasons":[],"originalAccept":{"scheme":"exact","network":"eip155:56","asset":"0x55d398326f99059fF775485246999027B3197955","amount":"10000","payTo":"0x1111111111111111111111111111111111111111"}}]}})";
            TemporaryExecutable baw(fake_cli(missing_status));
            BinanceAgenticWalletCliBridge bridge(
                {baw.path(), std::chrono::seconds(2)});

            EXPECT_THROW(
                static_cast<void>(bridge.preview(test_requirement())),
                BinanceAgenticWalletResponseError);
        }

        TEST(BinanceAgenticWalletBridgeTest, DistinguishesMissingSession) {
            TemporaryExecutable baw(
                "#!/bin/sh\n"
                "printf '%s' '{\"success\":true,\"data\":{\"status\":\"UNCONNECTED\"}}'\n");
            BinanceAgenticWalletCliBridge bridge(
                {baw.path(), std::chrono::seconds(2)});

            EXPECT_THROW(
                static_cast<void>(bridge.preview(test_requirement())),
                BinanceAgenticWalletNoSessionError);
        }

        TEST(BinanceAgenticWalletBridgeTest, DistinguishesExpiredSession) {
            TemporaryExecutable baw(
                "#!/bin/sh\n"
                "printf '%s' '{\"success\":false,\"error\":{\"name\":\"SESSION_EXPIRED\"}}'\n"
                "exit 1\n");
            BinanceAgenticWalletCliBridge bridge(
                {baw.path(), std::chrono::seconds(2)});

            EXPECT_THROW(
                static_cast<void>(bridge.preview(test_requirement())),
                BinanceAgenticWalletAuthenticationError);
        }

        TEST(BinanceAgenticWalletBridgeTest, DistinguishesMalformedProviderResponse) {
            TemporaryExecutable baw(fake_cli("not-json"));
            BinanceAgenticWalletCliBridge bridge(
                {baw.path(), std::chrono::seconds(2)});

            EXPECT_THROW(
                static_cast<void>(bridge.preview(test_requirement())),
                BinanceAgenticWalletResponseError);
        }

        TEST(BinanceAgenticWalletBridgeTest, DistinguishesCliTransportFailure) {
            BinanceAgenticWalletCliBridge bridge(
                {"/definitely/missing/baw", std::chrono::seconds(2)});

            EXPECT_THROW(
                static_cast<void>(bridge.preview(test_requirement())),
                BinanceAgenticWalletTransportError);
        }

        TEST(PreviewAuthorizedAccessGateTest,
             ApprovedUnlocksAndRejectedKeepsDemoOutputLocked) {
            PreviewAuthorizedAccessGate gate("{\"signal\":\"BUY\"}");
            ExternalPaymentPreview rejected{
                ExternalPaymentPreviewStatus::Rejected,
                "fake-binance-agentic-wallet",
                "eip155:97",
                "Mock U",
                "0.01",
                "ACTION_REQUIRED",
                {"INSUFFICIENT_BALANCE"}};

            EXPECT_FALSE(gate.apply_preview(rejected));
            EXPECT_FALSE(gate.preview_authorized());
            EXPECT_FALSE(gate.access().has_value());

            ExternalPaymentPreview approved = rejected;
            approved.status = ExternalPaymentPreviewStatus::Approved;
            approved.provider_status = "READY_TO_SIGN";
            approved.reasons.clear();
            EXPECT_TRUE(gate.apply_preview(approved));
            EXPECT_TRUE(gate.preview_authorized());
            EXPECT_EQ(gate.access(), "{\"signal\":\"BUY\"}");
        }

        TEST(BinanceAgenticWalletIntegrationTest,
             X402PreviewAuthorizationUnlocksOnlyDemoOutputWithoutCoreMutation) {
            AccountStore accounts;
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 500);
            const auto balance_before = accounts.find_balance(1, 10);

            OrderBook order_book;
            ASSERT_TRUE(order_book.add_order(
                Order{1, Side::Sell, OrderType::Limit, 100, 2, 1}).empty());
            const auto order_before = order_book.find_order(1);
            const std::size_t order_count_before = order_book.order_count();

            Ledger ledger;
            const auto ledger_before = ledger.entries();

            X402PaidService service(localhost_config());
            auto server = std::async(std::launch::async, [&service] {
                service.serve_once();
            });
            CurlX402Client client;
            const X402RequestResult challenge = client.get(
                service.resource_url());
            ASSERT_EQ(server.wait_for(std::chrono::seconds(2)),
                      std::future_status::ready);
            ASSERT_NO_THROW(server.get());
            ASSERT_EQ(challenge.http_status, 402);
            ASSERT_TRUE(challenge.payment_required.has_value());

            FakeBinanceAgenticWalletBridge wallet(
                ExternalPaymentPreview{
                    ExternalPaymentPreviewStatus::Approved,
                    "fake-binance-agentic-wallet",
                    challenge.payment_required->network,
                    "Mock U",
                    "0.01",
                    "READY_TO_SIGN",
                    {}});
            PreviewAuthorizedAccessGate gate("{\"signal\":\"BUY\"}");
            EXPECT_FALSE(gate.access().has_value());

            const ExternalPaymentPreview preview = wallet.preview(
                *challenge.payment_required);
            EXPECT_TRUE(gate.apply_preview(preview));
            EXPECT_EQ(gate.access(), "{\"signal\":\"BUY\"}");
            ASSERT_EQ(wallet.call_count(), 1U);
            ASSERT_TRUE(wallet.last_requirement().has_value());
            EXPECT_EQ(*wallet.last_requirement(), *challenge.payment_required);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_EQ(order_book.order_count(), order_count_before);
            const auto order_after = order_book.find_order(1);
            ASSERT_TRUE(order_before.has_value());
            ASSERT_TRUE(order_after.has_value());
            EXPECT_EQ(order_after->id, order_before->id);
            EXPECT_EQ(order_after->side, order_before->side);
            EXPECT_EQ(order_after->type, order_before->type);
            EXPECT_EQ(order_after->price, order_before->price);
            EXPECT_EQ(order_after->quantity, order_before->quantity);
            EXPECT_EQ(order_after->timestamp, order_before->timestamp);
            EXPECT_EQ(ledger.entries(), ledger_before);
        }

        TEST(BinanceAgenticWalletIntegrationTest,
             RejectedPreviewLeavesDemoOutputLockedWithoutCoreMutation) {
            AccountStore accounts;
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 500);
            const auto balance_before = accounts.find_balance(1, 10);
            OrderBook order_book;
            Ledger ledger;

            FakeBinanceAgenticWalletBridge wallet(
                ExternalPaymentPreview{
                    ExternalPaymentPreviewStatus::Rejected,
                    "fake-binance-agentic-wallet",
                    "eip155:97",
                    "Mock U",
                    "0.01",
                    "ACTION_REQUIRED",
                    {"INSUFFICIENT_BALANCE"}});
            PreviewAuthorizedAccessGate gate("premium output");

            EXPECT_FALSE(gate.apply_preview(wallet.preview(test_requirement())));
            EXPECT_FALSE(gate.access().has_value());
            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_EQ(order_book.order_count(), 0U);
            EXPECT_TRUE(ledger.entries().empty());
        }
    }  // namespace
}  // namespace exchange
