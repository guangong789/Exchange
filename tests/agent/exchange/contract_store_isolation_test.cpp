#include "../../support/wal_files.hpp"
#include "agent/domain/contract_store.hpp"
#include "agent/domain/utility.hpp"
#include "agent/exchange/agent_observation_service.hpp"
#include "durability/execution_wal.hpp"
#include "execution/trading_runtime.hpp"

#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr AgentId proposer = 101;
        constexpr AgentId counterparty = 202;

        TEST(ContractStoreIsolationTest,
             ContractLifecycleDoesNotMutateExchangeOrUtilityState) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            const TradingBootstrapConfig bootstrap{{
                BootstrapAccount{
                    1,
                    {{instrument.base_asset, {3, 0}},
                     {instrument.quote_asset, {1'000, 0}}}},
                BootstrapAccount{2, {}},
            }};
            test::TempTestDirectory directory;
            const std::string wal_path = directory.wal_path();
            std::unique_ptr<TradingRuntime> runtime =
                TradingRuntime::create_durable(
                    instrument,
                    wal_path,
                    bootstrap);
            const TradingRuntime& runtime_view = *runtime;
            AgentRegistry registry;
            ASSERT_TRUE(registry.register_agent({proposer, 1}));
            ASSERT_TRUE(registry.register_agent({counterparty, 2}));
            ContractStore contracts;
            AgentObservationService observations{
                registry,
                runtime_view.accounts(),
                runtime->reservations(),
                runtime->order_book(),
                contracts,
                instrument};
            const AgentPreferenceProfile preference{3, 100, 10};
            const WorldState world = observations.capture_world(1);
            const AgentObservation before = observations.observe(
                proposer,
                world,
                std::nullopt);
            const AgentUtilityBreakdown utility_before =
                evaluate_agent_utility(before, preference);

            ASSERT_EQ(
                contracts.create_contract(
                    1,
                    proposer,
                    counterparty,
                    ContractTerms{proposer, counterparty, 500,
                                  ResourceKind::ComputeCredit, 20}),
                ContractResult::Success);
            ASSERT_EQ(
                contracts.accept_contract(
                    1,
                    counterparty),
                ContractResult::Success);
            ASSERT_EQ(
                contracts.mark_fulfilled(
                    1,
                    counterparty),
                ContractResult::Success);
            ASSERT_EQ(
                contracts.mark_settled(1, proposer),
                ContractResult::Success);

            const AgentObservation after = observations.observe(
                proposer,
                observations.capture_world(2),
                std::nullopt);
            EXPECT_EQ(
                evaluate_agent_utility(after, preference),
                utility_before);
            EXPECT_EQ(before.base_balance, after.base_balance);
            EXPECT_EQ(before.quote_balance, after.quote_balance);
            EXPECT_EQ(runtime->order_book().order_count(), 0U);
            EXPECT_TRUE(runtime->reservations().entries().empty());
            EXPECT_TRUE(runtime->ledger().entries().empty());
            EXPECT_EQ(
                std::filesystem::file_size(wal_path),
                kWalFileHeaderEncodedSize);
        }
    }  // namespace
}  // namespace exchange
