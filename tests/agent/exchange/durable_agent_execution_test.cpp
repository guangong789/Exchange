#include "agent/domain/agent_registry.hpp"
#include "agent/exchange/trading_request_agent_execution_adapter.hpp"
#include "execution/trading_runtime.hpp"

#include <cerrno>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <variant>

#include <unistd.h>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
        constexpr AgentId agent_id = 101;
        constexpr AccountId account_id = 1;

        class TemporaryDirectory {
        public:
            TemporaryDirectory() {
                std::string pattern =
                    "/tmp/exchange-agent-durable-test-XXXXXX";
                path_ = ::mkdtemp(pattern.data());
                if (path_.empty()) {
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "mkdtemp");
                }
            }

            ~TemporaryDirectory() {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }

            [[nodiscard]] std::string wal_path() const {
                return path_ + "/execution.wal";
            }

        private:
            std::string path_;
        };

        TradingBootstrapConfig funded_bootstrap() {
            return TradingBootstrapConfig{{BootstrapAccount{
                account_id,
                {{instrument.quote_asset, {1'000, 0}}}}}};
        }

        TEST(DurableAgentExecutionTest,
             AgentSubmitUsesDurableExecutorAndRecoversState) {
            TemporaryDirectory directory;
            const std::string wal_path = directory.wal_path();
            AgentRegistry registry;
            ASSERT_TRUE(registry.register_agent({agent_id, account_id}));

            {
                std::unique_ptr<TradingRuntime> runtime =
                    TradingRuntime::create_durable(
                        instrument,
                        wal_path,
                        funded_bootstrap());
                TradingRequestAgentExecutionAdapter adapter{
                    registry,
                    runtime->executor()};

                const AgentActionResult result = adapter.execute(
                    agent_id,
                    SubmitOrderAction{Side::Buy, 100, 2});

                ASSERT_TRUE(
                    std::holds_alternative<SubmitActionResult>(result));
                EXPECT_EQ(
                    std::get<SubmitActionResult>(result),
                    (SubmitActionResult{
                        1,
                        1,
                        AgentSubmitStatus::Accepted}));
                EXPECT_TRUE(runtime->order_book().find_order(1).has_value());
            }

            const std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(
                    instrument,
                    wal_path,
                    funded_bootstrap());
            const TradingRuntime& recovered_view = *recovered;
            ASSERT_TRUE(recovered->order_book().find_order(1).has_value());
            EXPECT_EQ(
                recovered_view.accounts().find_balance(
                    account_id,
                    instrument.quote_asset),
                (Balance{800, 200}));
            EXPECT_EQ(
                recovered->reservations().find(1),
                (OrderReservation{account_id, instrument.quote_asset, 200, 200}));
            EXPECT_EQ(recovered->ledger().entries().size(), 1U);
        }
    }  // namespace
}  // namespace exchange
