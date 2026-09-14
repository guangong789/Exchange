#include "agent/domain/agent_registry.hpp"
#include "agent/exchange/agent_observation_service.hpp"
#include "agent/exchange/trading_request_agent_execution_adapter.hpp"
#include "agent/runtime/agent_runtime.hpp"
#include "durability/execution_wal.hpp"
#include "execution/trading_runtime.hpp"

#include <cerrno>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <variant>
#include <utility>

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

        class MutableDecisionProvider final : public AgentDecisionProvider {
        public:
            explicit MutableDecisionProvider(AgentAction action)
                : action(std::move(action)) {}

            AgentAction decide(const AgentObservation&) const override {
                return action;
            }

            AgentAction action;
        };

        TEST(DurableAgentExecutionTest,
             AgentSubmitUsesDurableExecutorAndRecoversState) {
            TemporaryDirectory directory;
            const std::string wal_path = directory.wal_path();

            {
                std::unique_ptr<TradingRuntime> runtime =
                    TradingRuntime::create_durable(
                        instrument,
                        wal_path,
                        funded_bootstrap());
                ASSERT_TRUE(runtime->agent_registry().register_agent(
                    {agent_id, account_id}));
                TradingRequestAgentExecutionAdapter adapter{
                    runtime->agent_registry(),
                    runtime->executor(),
                    runtime->contract_executor()};

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

        TEST(DurableAgentExecutionTest,
             EconomicRejectionWritesNoWalAndConsumesNoExecutionIdentity) {
            TemporaryDirectory directory;
            const std::string wal_path = directory.wal_path();
            MutableDecisionProvider provider{
                SubmitOrderAction{Side::Buy, 101, 1}};
            AgentEconomicProfile profile;
            profile.max_buy_price = 100;

            {
                std::unique_ptr<TradingRuntime> runtime =
                    TradingRuntime::create_durable(
                        instrument,
                        wal_path,
                        funded_bootstrap());
                ASSERT_TRUE(runtime->agent_registry().register_agent(
                    {agent_id, account_id}));
                const TradingRuntime& runtime_view = *runtime;
                AgentObservationService observations{
                    runtime->agent_registry(),
                    runtime_view.accounts(),
                    runtime->reservations(),
                    runtime->order_book(),
                    runtime->contracts(),
                    instrument};
                TradingRequestAgentExecutionAdapter adapter{
                    runtime->agent_registry(),
                    runtime->executor(),
                    runtime->contract_executor()};
                AgentRuntime agent_runtime(
                    {{agent_id,
                      &provider,
                      std::nullopt,
                      profile,
                      AgentPreferenceProfile{0, 100, 0}}},
                    observations,
                    adapter,
                    instrument);

                agent_runtime.run_step_at(1);

                ASSERT_EQ(agent_runtime.trace().size(), 1U);
                EXPECT_EQ(
                    agent_runtime.trace().front().status,
                    AgentTurnStatus::EconomicConstraintRejected);
                EXPECT_EQ(
                    agent_runtime.trace().front().economic_constraint,
                    AgentEconomicConstraintResult::BuyPriceExceeded);
                EXPECT_EQ(
                    agent_runtime.trace().front().utility_delta,
                    0);
                EXPECT_EQ(
                    std::filesystem::file_size(wal_path),
                    kWalFileHeaderEncodedSize);
                EXPECT_EQ(runtime->order_book().order_count(), 0U);
                EXPECT_TRUE(runtime->reservations().entries().empty());
                EXPECT_TRUE(runtime->ledger().entries().empty());
                EXPECT_EQ(
                    runtime_view.accounts().find_balance(
                        account_id,
                        instrument.quote_asset),
                    (Balance{1'000, 0}));
                const PerAgentExperimentMetrics* rejected_metrics =
                    agent_runtime.metrics().find_agent(agent_id);
                ASSERT_NE(rejected_metrics, nullptr);
                EXPECT_EQ(
                    rejected_metrics->economic_constraint_rejections,
                    1U);
                EXPECT_EQ(rejected_metrics->successful_executions, 0U);

                provider.action = SubmitOrderAction{Side::Buy, 100, 1};
                agent_runtime.run_step_at(2);

                ASSERT_EQ(agent_runtime.trace().size(), 2U);
                const AgentTurnRecord& allowed = agent_runtime.trace().back();
                EXPECT_EQ(allowed.status, AgentTurnStatus::Executed);
                EXPECT_EQ(
                    allowed.economic_constraint,
                    AgentEconomicConstraintResult::Allowed);
                ASSERT_TRUE(allowed.execution_result.has_value());
                EXPECT_EQ(
                    std::get<SubmitActionResult>(
                        *allowed.execution_result),
                    (SubmitActionResult{
                        1,
                        1,
                        AgentSubmitStatus::Accepted}));
                EXPECT_EQ(
                    std::filesystem::file_size(wal_path),
                    kWalFileHeaderEncodedSize
                        + kWalSubmitRecordEncodedSize);
                const PerAgentExperimentMetrics* metrics =
                    agent_runtime.metrics().find_agent(agent_id);
                ASSERT_NE(metrics, nullptr);
                EXPECT_EQ(metrics->turns, 2U);
                EXPECT_EQ(metrics->successful_executions, 1U);
                EXPECT_EQ(metrics->total_accepted_quantity, 1);
                EXPECT_EQ(
                    metrics->final_state.quote_balance,
                    (Balance{900, 100}));
            }

            const std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(
                    instrument,
                    wal_path,
                    funded_bootstrap());
            EXPECT_TRUE(recovered->order_book().find_order(1).has_value());
            EXPECT_EQ(recovered->ledger().entries().size(), 1U);
        }

        TEST(DurableAgentExecutionTest,
             HoldRecordingDoesNotWriteWalOrMutateExchangeState) {
            TemporaryDirectory directory;
            const std::string wal_path = directory.wal_path();
            MutableDecisionProvider provider{HoldAction{}};
            std::unique_ptr<TradingRuntime> runtime =
                TradingRuntime::create_durable(
                    instrument,
                    wal_path,
                    funded_bootstrap());
            ASSERT_TRUE(runtime->agent_registry().register_agent(
                {agent_id, account_id}));
            const TradingRuntime& runtime_view = *runtime;
            AgentObservationService observations{
                runtime->agent_registry(),
                runtime_view.accounts(),
                runtime->reservations(),
                runtime->order_book(),
                runtime->contracts(),
                instrument};
            TradingRequestAgentExecutionAdapter adapter{
                runtime->agent_registry(),
                runtime->executor(),
                runtime->contract_executor()};
            AgentRuntime agent_runtime(
                {{agent_id,
                  &provider,
                  std::nullopt,
                  {},
                  AgentPreferenceProfile{0, 100, 0}}},
                observations,
                adapter,
                instrument);

            agent_runtime.run_step_at(1);

            ASSERT_EQ(agent_runtime.trace().size(), 1U);
            EXPECT_EQ(
                agent_runtime.trace().front().status,
                AgentTurnStatus::Held);
            EXPECT_EQ(agent_runtime.trace().front().utility_delta, 0);
            EXPECT_EQ(
                std::filesystem::file_size(wal_path),
                kWalFileHeaderEncodedSize);
            EXPECT_EQ(runtime->order_book().order_count(), 0U);
            EXPECT_TRUE(runtime->reservations().entries().empty());
            EXPECT_TRUE(runtime->ledger().entries().empty());
            EXPECT_EQ(
                runtime_view.accounts().find_balance(
                    account_id,
                    instrument.quote_asset),
                (Balance{1'000, 0}));
            const PerAgentExperimentMetrics* metrics =
                agent_runtime.metrics().find_agent(agent_id);
            ASSERT_NE(metrics, nullptr);
            EXPECT_EQ(metrics->turns, 1U);
            EXPECT_EQ(metrics->holds, 1U);
            EXPECT_EQ(metrics->successful_executions, 0U);
        }
    }  // namespace
}  // namespace exchange
