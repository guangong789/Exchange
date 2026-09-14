#include "execution/trading_runtime.hpp"

#include <cerrno>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <variant>

#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
        constexpr AgentId proposer = 101;
        constexpr AgentId counterparty = 202;

        class TemporaryDirectory {
        public:
            TemporaryDirectory() {
                std::string pattern =
                    "/tmp/exchange-contract-process-test-XXXXXX";
                const char* created = ::mkdtemp(pattern.data());
                if (created == nullptr) {
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "mkdtemp");
                }
                path_ = created;
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

        TradingBootstrapConfig bootstrap() {
            return TradingBootstrapConfig{{
                BootstrapAccount{
                    1,
                    {{instrument.quote_asset, {1'000, 0}}}},
                BootstrapAccount{
                    2,
                    {{instrument.quote_asset, {100, 0}}}},
            }};
        }

        bool register_agents(TradingRuntime& runtime) {
            return runtime.agent_registry().register_agent({proposer, 1})
                && runtime.agent_registry().register_agent(
                    {counterparty, 2});
        }

        CreateContractRequest request() {
            return CreateContractRequest{
                proposer,
                counterparty,
                ContractTerms{
                    proposer,
                    counterparty,
                    500,
                    ResourceKind::ComputeCredit,
                    20}};
        }

        enum class CrashState {
            Accepted,
            Fulfilled,
            Settled,
            TwoProposed,
        };

        void run_crashing_child(
            const std::string& path,
            CrashState target) {
            const pid_t child = ::fork();
            ASSERT_NE(child, -1);
            if (child == 0) {
                try {
                    std::unique_ptr<TradingRuntime> runtime =
                        TradingRuntime::create_durable(
                            instrument,
                            path,
                            bootstrap());
                    if (!register_agents(*runtime)) {
                        _exit(101);
                    }
                    const ContractExecutionResponse first =
                        runtime->contract_executor().create_contract(
                            request());
                    if (first.result != ContractResult::Success
                        || first.contract_id != 1U) {
                        _exit(102);
                    }
                    if (target == CrashState::TwoProposed) {
                        const ContractExecutionResponse second =
                            runtime->contract_executor().create_contract(
                                request());
                        if (second.result != ContractResult::Success
                            || second.contract_id != 2U) {
                            _exit(103);
                        }
                        _exit(0);
                    }
                    if (runtime->contract_executor().accept_contract(
                            1,
                            counterparty)
                        != ContractResult::Success) {
                        _exit(104);
                    }
                    if (target == CrashState::Accepted) {
                        _exit(0);
                    }
                    if (runtime->contract_executor().fulfill_resource(
                            1,
                            counterparty)
                        != ContractResult::Success) {
                        _exit(105);
                    }
                    if (target == CrashState::Fulfilled) {
                        _exit(0);
                    }
                    if (runtime->contract_executor().settle_payment(
                            1,
                            proposer)
                        != ContractResult::Success) {
                        _exit(106);
                    }
                    _exit(0);
                } catch (...) {
                    _exit(107);
                }
            }

            int status = 0;
            pid_t waited = -1;
            do {
                waited = ::waitpid(child, &status, 0);
            } while (waited == -1 && errno == EINTR);
            ASSERT_EQ(waited, child);
            ASSERT_TRUE(WIFEXITED(status));
            ASSERT_EQ(WEXITSTATUS(status), 0);
        }

        void expect_recovered_state(
            CrashState crashed_state,
            ContractState expected_state) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            run_crashing_child(path, crashed_state);

            const std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            const std::optional<Contract> contract =
                recovered->contracts().find(1);
            ASSERT_TRUE(contract.has_value());
            EXPECT_EQ(contract->state, expected_state);
            EXPECT_EQ(
                contract->resource_delivery_obligation.fulfilled,
                expected_state == ContractState::Fulfilled
                    || expected_state == ContractState::Settled);
            EXPECT_EQ(
                contract->payment_obligation.fulfilled,
                expected_state == ContractState::Settled);
            const Amount expected_payer =
                expected_state == ContractState::Settled ? 500 : 1'000;
            const Amount expected_payee =
                expected_state == ContractState::Settled ? 600 : 100;
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().find_balance(
                    1, instrument.quote_asset),
                (Balance{expected_payer, 0}));
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().find_balance(
                    2, instrument.quote_asset),
                (Balance{expected_payee, 0}));
            EXPECT_EQ(
                recovered->ledger().entries().size(),
                expected_state == ContractState::Settled ? 1U : 0U);
            if (expected_state == ContractState::Settled) {
                ASSERT_TRUE(std::holds_alternative<
                            ContractSettlementLedgerMetadata>(
                    recovered->ledger().entries().front()
                        .transaction.metadata));
                EXPECT_EQ(
                    recovered->ledger().entries().front().sequence,
                    1U);
            }
        }

        TEST(DurableContractProcessTest, AcceptedContractSurvivesCrash) {
            expect_recovered_state(
                CrashState::Accepted,
                ContractState::Accepted);
        }

        TEST(DurableContractProcessTest, FulfilledContractSurvivesCrash) {
            expect_recovered_state(
                CrashState::Fulfilled,
                ContractState::Fulfilled);
        }

        TEST(DurableContractProcessTest, SettledContractSurvivesCrash) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            run_crashing_child(path, CrashState::Settled);

            std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            ASSERT_EQ(recovered->contracts().find(1)->state,
                      ContractState::Settled);
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().find_balance(
                    1, instrument.quote_asset),
                (Balance{500, 0}));
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().find_balance(
                    2, instrument.quote_asset),
                (Balance{600, 0}));
            ASSERT_EQ(recovered->ledger().entries().size(), 1U);

            ASSERT_TRUE(register_agents(*recovered));
            EXPECT_EQ(
                recovered->contract_executor().settle_payment(1, proposer),
                ContractResult::InvalidTransition);
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().find_balance(
                    1, instrument.quote_asset),
                (Balance{500, 0}));
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().find_balance(
                    2, instrument.quote_asset),
                (Balance{600, 0}));
            EXPECT_EQ(recovered->ledger().entries().size(), 1U);
        }

        TEST(DurableContractProcessTest, NextContractIdSurvivesCrash) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            run_crashing_child(path, CrashState::TwoProposed);

            std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            ASSERT_EQ(recovered->contracts().size(), 2U);
            ASSERT_TRUE(register_agents(*recovered));
            const ContractExecutionResponse next =
                recovered->contract_executor().create_contract(request());
            EXPECT_EQ(next.result, ContractResult::Success);
            EXPECT_EQ(next.contract_id, 3U);
        }
    }  // namespace
}  // namespace exchange
