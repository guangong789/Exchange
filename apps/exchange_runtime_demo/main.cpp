#include "agent/domain/agent.hpp"
#include "agent/exchange/agent_observation_service.hpp"
#include "agent/exchange/trading_request_agent_execution_adapter.hpp"
#include "agent/runtime/agent_runtime.hpp"
#include "durability/execution_wal.hpp"
#include "execution/execution_command.hpp"
#include "execution/trading_bootstrap.hpp"
#include "execution/trading_request.hpp"
#include "execution/trading_runtime.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
    constexpr exchange::InstrumentContext instrument{1, 2, 1, 1, 1};
    constexpr exchange::AgentId buyer_agent = 101;
    constexpr exchange::AgentId seller_agent = 202;
    constexpr exchange::AccountId buyer_account = 1;
    constexpr exchange::AccountId seller_account = 2;
    constexpr exchange::RequestId ambiguous_request_id = 40;

    class ScopedFd {
    public:
        explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}

        ~ScopedFd() {
            if (fd_ != -1) {
                static_cast<void>(::close(fd_));
            }
        }

        ScopedFd(const ScopedFd&) = delete;
        ScopedFd& operator=(const ScopedFd&) = delete;

        ScopedFd(ScopedFd&& other) noexcept
            : fd_(std::exchange(other.fd_, -1)) {}

        ScopedFd& operator=(ScopedFd&& other) noexcept {
            if (this != &other) {
                if (fd_ != -1) {
                    static_cast<void>(::close(fd_));
                }
                fd_ = std::exchange(other.fd_, -1);
            }
            return *this;
        }

        [[nodiscard]] int get() const noexcept { return fd_; }

    private:
        int fd_;
    };

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            std::string pattern =
                (std::filesystem::temp_directory_path()
                 / "exchange-runtime-demo-XXXXXX")
                    .string();
            char* const created = ::mkdtemp(pattern.data());
            if (created == nullptr) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "create demo temporary directory");
            }
            path_ = created;
        }

        ~TemporaryDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        [[nodiscard]] std::string wal_path() const {
            return path_ + "/execution.wal";
        }

    private:
        std::string path_;
    };

    class FixedDecisionProvider final : public exchange::AgentDecisionProvider {
    public:
        explicit FixedDecisionProvider(exchange::AgentAction action)
            : action_(std::move(action)) {}

        [[nodiscard]] exchange::AgentAction decide(
            const exchange::AgentObservation&) const override {
            return action_;
        }

    private:
        exchange::AgentAction action_;
    };

    struct StateSummary {
        exchange::AccountStore::AccountBalances accounts;
        std::map<exchange::OrderId, exchange::OrderReservation> reservations;
        std::vector<exchange::LedgerEntry> ledger;
        std::size_t order_count{};
        std::size_t contract_count{};

        bool operator==(const StateSummary&) const = default;
    };

    [[noreturn]] void fail(const std::string& message) {
        throw std::runtime_error(message);
    }

    void require(bool condition, const std::string& message) {
        if (!condition) {
            fail(message);
        }
    }

    exchange::TradingBootstrapConfig bootstrap() {
        return exchange::TradingBootstrapConfig{{
            exchange::BootstrapAccount{
                buyer_account,
                {{instrument.quote_asset, {2'000, 0}}}},
            exchange::BootstrapAccount{
                seller_account,
                {{instrument.base_asset, {10, 0}},
                 {instrument.quote_asset, {100, 0}}}},
        }};
    }

    void register_agents(exchange::TradingRuntime& runtime) {
        require(
            runtime.agent_registry().register_agent(
                {buyer_agent, buyer_account}),
            "failed to register buyer Agent");
        require(
            runtime.agent_registry().register_agent(
                {seller_agent, seller_account}),
            "failed to register seller Agent");
    }

    std::vector<std::uint8_t> read_file(const std::string& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            fail("failed to open demo WAL");
        }
        const std::streamsize size = input.tellg();
        require(size >= 0, "failed to determine demo WAL size");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        if (size != 0
            && !input.read(
                reinterpret_cast<char*>(bytes.data()),
                size)) {
            fail("failed to read demo WAL");
        }
        return bytes;
    }

    exchange::WalScanResult scan_wal(const std::string& path) {
        exchange::WalScanResult scan = exchange::scan_execution_wal(
            read_file(path),
            instrument,
            exchange::calculate_bootstrap_fingerprint(bootstrap()));
        require(
            scan.status == exchange::WalScanStatus::CleanEof,
            "demo WAL did not end at a clean record boundary");
        return scan;
    }

    StateSummary summarize(const exchange::TradingRuntime& runtime) {
        return StateSummary{
            runtime.accounts().entries(),
            runtime.reservations().entries(),
            runtime.ledger().entries(),
            runtime.order_book().order_count(),
            runtime.contracts().size()};
    }

    exchange::Balance balance(
        const exchange::TradingRuntime& runtime,
        exchange::AccountId account_id,
        exchange::AssetId asset_id) {
        const std::optional<exchange::Balance> value =
            runtime.accounts().find_balance(account_id, asset_id);
        require(value.has_value(), "expected balance is missing");
        return *value;
    }

    void print_account(
        const char* label,
        const exchange::TradingRuntime& runtime,
        exchange::AccountId account_id) {
        const exchange::Balance base = balance(
            runtime, account_id, instrument.base_asset);
        const exchange::Balance quote = balance(
            runtime, account_id, instrument.quote_asset);
        std::cout << label << " account=" << account_id
                  << " base=" << base.available << "/" << base.reserved
                  << " quote=" << quote.available << "/" << quote.reserved
                  << " (available/reserved)\n";
    }

    std::string_view contract_state_name(exchange::ContractState state) {
        using State = exchange::ContractState;
        switch (state) {
            case State::Proposed: return "Proposed";
            case State::Accepted: return "Accepted";
            case State::Rejected: return "Rejected";
            case State::Fulfilled: return "Fulfilled";
            case State::Settled: return "Settled";
        }
        fail("unknown contract state");
    }

    std::string_view turn_status_name(exchange::AgentTurnStatus status) {
        using Status = exchange::AgentTurnStatus;
        switch (status) {
            case Status::DecisionFailed: return "DecisionFailed";
            case Status::StructuralValidationRejected:
                return "StructuralValidationRejected";
            case Status::EconomicConstraintRejected:
                return "EconomicConstraintRejected";
            case Status::ExecutionRejected: return "ExecutionRejected";
            case Status::Executed: return "Executed";
            case Status::Held: return "Held";
        }
        fail("unknown Agent turn status");
    }

    std::string_view contract_result_name(exchange::ContractResult result) {
        using Result = exchange::ContractResult;
        switch (result) {
            case Result::Success: return "Success";
            case Result::ContractNotFound: return "ContractNotFound";
            case Result::InvalidTerms: return "InvalidTerms";
            case Result::InvalidTransition: return "InvalidTransition";
            case Result::UnauthorizedActor: return "UnauthorizedActor";
            case Result::AccountNotFound: return "AccountNotFound";
            case Result::InsufficientFunds: return "InsufficientFunds";
            case Result::BalanceOverflow: return "BalanceOverflow";
            case Result::ContractIdExhausted: return "ContractIdExhausted";
        }
        fail("unknown contract result");
    }

    const exchange::SubmitActionResult& submit_result(
        const exchange::AgentTurnRecord& turn) {
        require(turn.execution_result.has_value(), "Agent turn has no result");
        const auto* result = std::get_if<exchange::SubmitActionResult>(
            &*turn.execution_result);
        require(result != nullptr, "Agent turn result is not a submit result");
        return *result;
    }

    const exchange::Trade& find_trade(const exchange::Ledger& ledger) {
        for (const exchange::LedgerEntry& entry : ledger.entries()) {
            const auto* metadata = std::get_if<exchange::TradeLedgerMetadata>(
                &entry.transaction.metadata);
            if (metadata != nullptr) {
                return metadata->trade;
            }
        }
        fail("expected trade Ledger entry is missing");
    }

    const exchange::LedgerEntry& find_contract_settlement(
        const exchange::Ledger& ledger,
        exchange::ContractId contract_id) {
        for (const exchange::LedgerEntry& entry : ledger.entries()) {
            const auto* metadata = std::get_if<
                exchange::ContractSettlementLedgerMetadata>(
                &entry.transaction.metadata);
            if (metadata != nullptr && metadata->contract_id == contract_id) {
                return entry;
            }
        }
        fail("expected contract settlement Ledger entry is missing");
    }

    void write_one(int fd, char value) {
        while (true) {
            const ssize_t count = ::write(fd, &value, 1);
            if (count == 1) {
                return;
            }
            if (count == -1 && errno == EINTR) {
                continue;
            }
            fail("failed to write process synchronization byte");
        }
    }

    char read_one(int fd) {
        char value{};
        while (true) {
            const ssize_t count = ::read(fd, &value, 1);
            if (count == 1) {
                return value;
            }
            if (count == -1 && errno == EINTR) {
                continue;
            }
            fail("failed to read process synchronization byte");
        }
    }

    void wait_for_child(pid_t child) {
        int status = 0;
        pid_t waited = -1;
        do {
            waited = ::waitpid(child, &status, 0);
        } while (waited == -1 && errno == EINTR);
        require(waited == child, "failed to wait for demo child process");
        require(
            WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
            "demo child did not terminate at the intended crash point");
    }

    void execute_ambiguous_request_then_crash(const std::string& wal_path) {
        int application_channel[2]{};
        int durable_checkpoint[2]{};
        if (::socketpair(
                AF_UNIX,
                SOCK_STREAM | SOCK_CLOEXEC,
                0,
                application_channel)
            == -1) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "create demo application channel");
        }
        ScopedFd parent_application{application_channel[0]};
        ScopedFd child_application{application_channel[1]};
        if (::pipe2(durable_checkpoint, O_CLOEXEC) == -1) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "create demo durability checkpoint");
        }
        ScopedFd checkpoint_read{durable_checkpoint[0]};
        ScopedFd checkpoint_write{durable_checkpoint[1]};

        const pid_t child = ::fork();
        if (child == -1) {
            throw std::system_error(
                errno, std::generic_category(), "fork demo child");
        }
        if (child == 0) {
            parent_application = ScopedFd{};
            checkpoint_read = ScopedFd{};
            try {
                require(
                    read_one(child_application.get()) == 'R',
                    "demo child received an invalid request marker");
                std::unique_ptr<exchange::TradingRuntime> runtime =
                    exchange::TradingRuntime::create_durable(
                        instrument,
                        wal_path,
                        bootstrap());
                const exchange::TradingResponse response =
                    runtime->executor().execute(exchange::TradingRequest{
                        ambiguous_request_id,
                        seller_account,
                        exchange::SubmitTradingRequest{
                            exchange::Side::Sell,
                            110,
                            2}});
                require(
                    response.result == exchange::TradingResult::Accepted
                        && response.assigned_order_id == 3,
                    "ambiguous demo command was not accepted as order 3");

                // This byte is an internal demo checkpoint, not the business
                // response. It lets the parent kill the process only after the
                // real WAL-backed executor has completed the command.
                write_one(checkpoint_write.get(), 'D');
                while (true) {
                    ::pause();
                }
            } catch (...) {
                _exit(1);
            }
        }

        child_application = ScopedFd{};
        checkpoint_write = ScopedFd{};
        try {
            write_one(parent_application.get(), 'R');
            require(
                read_one(checkpoint_read.get()) == 'D',
                "demo child missed the durable checkpoint");
            if (::kill(child, SIGKILL) == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "kill demo child");
            }
            wait_for_child(child);

            char response_byte{};
            ssize_t response_count = -1;
            do {
                response_count = ::read(
                    parent_application.get(), &response_byte, 1);
            } while (response_count == -1 && errno == EINTR);
            require(
                response_count == 0,
                "ambiguous demo application unexpectedly received a response");
        } catch (...) {
            static_cast<void>(::kill(child, SIGKILL));
            int ignored = 0;
            while (::waitpid(child, &ignored, 0) == -1 && errno == EINTR) {
            }
            throw;
        }
    }

    void run_demo() {
        TemporaryDirectory directory;
        const std::string wal_path = directory.wal_path();
        const exchange::TradingBootstrapConfig initial_state = bootstrap();

        std::cout << "Deterministic Agent Execution Runtime\n"
                  << "Trust boundary: intent -> validate -> journal -> apply\n"
                  << "All assets and settlement in this demo are internal.\n\n";

        std::unique_ptr<exchange::TradingRuntime> runtime =
            exchange::TradingRuntime::create_durable(
                instrument, wal_path, initial_state);
        register_agents(*runtime);
        const exchange::TradingRuntime& view = *runtime;
        exchange::AgentObservationService observations{
            runtime->agent_registry(),
            view.accounts(),
            runtime->reservations(),
            runtime->order_book(),
            runtime->contracts(),
            instrument};
        exchange::TradingRequestAgentExecutionAdapter execution{
            runtime->agent_registry(),
            runtime->executor(),
            runtime->contract_executor()};

        std::cout << "=== Stage 1: Valid Typed Trading Actions ===\n";
        FixedDecisionProvider seller{
            exchange::SubmitOrderAction{exchange::Side::Sell, 100, 2}};
        FixedDecisionProvider buyer{
            exchange::SubmitOrderAction{exchange::Side::Buy, 100, 1}};
        exchange::AgentRuntime trading_agents(
            {{seller_agent, &seller, std::nullopt, {}, std::nullopt},
             {buyer_agent, &buyer, std::nullopt, {}, std::nullopt}},
            observations,
            execution,
            instrument);
        trading_agents.run_step_at(1);
        require(trading_agents.trace().size() == 2, "unexpected trade trace");
        const exchange::AgentTurnRecord& sell_turn = trading_agents.trace()[0];
        const exchange::AgentTurnRecord& buy_turn = trading_agents.trace()[1];
        const exchange::SubmitActionResult& sell_result =
            submit_result(sell_turn);
        const exchange::SubmitActionResult& buy_result =
            submit_result(buy_turn);
        require(
            sell_turn.status == exchange::AgentTurnStatus::Executed
                && buy_turn.status == exchange::AgentTurnStatus::Executed
                && sell_result.status == exchange::AgentSubmitStatus::Accepted
                && buy_result.status == exchange::AgentSubmitStatus::Accepted,
            "scripted trading action was not executed");
        const exchange::Trade& trade = find_trade(runtime->ledger());
        std::cout << "Intent: agent=" << seller_agent
                  << " submit SELL price=100 quantity=2\n"
                  << "Result: Accepted order_id=" << sell_result.order_id
                  << "\n"
                  << "Intent: agent=" << buyer_agent
                  << " submit BUY price=100 quantity=1\n"
                  << "Result: Accepted order_id=" << buy_result.order_id
                  << "\n"
                  << "Trade: buy_order=" << trade.buy_order_id
                  << " sell_order=" << trade.sell_order_id
                  << " price=" << trade.price
                  << " quantity=" << trade.quantity << "\n";
        print_account("Buyer", *runtime, buyer_account);
        print_account("Seller", *runtime, seller_account);
        const auto resting = runtime->order_book().find_order(1);
        const auto resting_reservation = runtime->reservations().find(1);
        require(
            resting.has_value() && resting->quantity == 1
                && resting_reservation.has_value()
                && resting_reservation->remaining_amount == 1,
            "expected partially filled resting order is missing");
        std::cout << "Resting order: id=1 remaining=1 reservation=1 base\n"
                  << "Ledger: entries=" << runtime->ledger().entries().size()
                  << " last_sequence="
                  << runtime->ledger().entries().back().sequence
                  << "\nWAL: records=" << scan_wal(wal_path).records.size()
                  << "\n\n";

        std::cout << "=== Stage 2: Invalid Intent Rejected ===\n";
        const StateSummary before_rejection = summarize(*runtime);
        const std::size_t wal_before_rejection =
            scan_wal(wal_path).records.size();
        FixedDecisionProvider excessive_price{
            exchange::SubmitOrderAction{exchange::Side::Buy, 101, 1}};
        exchange::AgentEconomicProfile restricted;
        restricted.max_buy_price = 100;
        exchange::AgentRuntime rejecting_agent(
            {{buyer_agent,
              &excessive_price,
              std::nullopt,
              restricted,
              std::nullopt}},
            observations,
            execution,
            instrument);
        rejecting_agent.run_step_at(2);
        require(rejecting_agent.trace().size() == 1, "unexpected rejection trace");
        const exchange::AgentTurnRecord& rejected =
            rejecting_agent.trace().front();
        require(
            rejected.status
                    == exchange::AgentTurnStatus::EconomicConstraintRejected
                && rejected.economic_constraint
                    == exchange::AgentEconomicConstraintResult::
                        BuyPriceExceeded,
            "invalid intent was not rejected by the economic constraint");
        const StateSummary after_rejection = summarize(*runtime);
        const std::size_t wal_after_rejection =
            scan_wal(wal_path).records.size();
        require(
            before_rejection == after_rejection
                && wal_before_rejection == wal_after_rejection,
            "rejected intent mutated authoritative state");
        std::cout << "Intent: agent=" << buyer_agent
                  << " submit BUY price=101 quantity=1, max_buy_price=100\n"
                  << "Result: " << turn_status_name(rejected.status)
                  << " reason=BuyPriceExceeded\n"
                  << "Authoritative state unchanged: true\n"
                  << "WAL records: " << wal_before_rejection << " -> "
                  << wal_after_rejection << "\n\n";

        std::cout << "=== Stage 3: Durable Bilateral Contract Settlement ===\n";
        const exchange::ContractTerms terms{
            buyer_agent,
            seller_agent,
            300,
            exchange::ResourceKind::ComputeCredit,
            5};
        const exchange::AgentActionResult proposed = execution.execute(
            buyer_agent,
            exchange::ProposeContractAction{seller_agent, terms});
        const auto* proposal_result =
            std::get_if<exchange::ContractActionResult>(&proposed);
        require(
            proposal_result != nullptr
                && proposal_result->status == exchange::ContractResult::Success
                && proposal_result->contract_id == 1,
            "contract proposal failed");
        const exchange::ContractId contract_id = *proposal_result->contract_id;
        const auto state_after_proposal = runtime->contracts().find(contract_id);
        require(state_after_proposal.has_value(), "proposed contract is missing");

        const exchange::AgentActionResult accepted = execution.execute(
            seller_agent,
            exchange::AcceptContractAction{contract_id});
        require(
            std::get<exchange::ContractActionResult>(accepted).status
                == exchange::ContractResult::Success,
            "contract acceptance failed");
        const exchange::ContractState accepted_state =
            runtime->contracts().find(contract_id)->state;

        const exchange::AgentActionResult fulfilled = execution.execute(
            seller_agent,
            exchange::FulfillResourceObligationAction{contract_id});
        require(
            std::get<exchange::ContractActionResult>(fulfilled).status
                == exchange::ContractResult::Success,
            "contract fulfillment failed");
        const exchange::ContractState fulfilled_state =
            runtime->contracts().find(contract_id)->state;

        const exchange::Balance payer_before = balance(
            *runtime, buyer_account, instrument.quote_asset);
        const exchange::Balance payee_before = balance(
            *runtime, seller_account, instrument.quote_asset);
        const exchange::AgentActionResult settled = execution.execute(
            buyer_agent,
            exchange::SettlePaymentObligationAction{contract_id});
        require(
            std::get<exchange::ContractActionResult>(settled).status
                == exchange::ContractResult::Success,
            "contract settlement failed");
        const exchange::Contract contract =
            *runtime->contracts().find(contract_id);
        const exchange::LedgerEntry& settlement = find_contract_settlement(
            runtime->ledger(), contract_id);
        const auto& settlement_metadata = std::get<
            exchange::ContractSettlementLedgerMetadata>(
            settlement.transaction.metadata);
        std::cout << "Agents: proposer/payer=" << buyer_agent
                  << " counterparty/payee=" << seller_agent << "\n"
                  << "Contract: id=" << contract_id
                  << " internal_quote=300 resource=ComputeCredit quantity=5\n"
                  << "Lifecycle: "
                  << contract_state_name(state_after_proposal->state) << " -> "
                  << contract_state_name(accepted_state) << " -> "
                  << contract_state_name(fulfilled_state) << " -> "
                  << contract_state_name(contract.state) << "\n"
                  << "Obligations: resource_fulfilled="
                  << std::boolalpha
                  << contract.resource_delivery_obligation.fulfilled
                  << " payment_fulfilled="
                  << contract.payment_obligation.fulfilled << "\n"
                  << "Payer quote: " << payer_before.available << " -> "
                  << balance(*runtime, buyer_account, instrument.quote_asset)
                         .available
                  << "\nPayee quote: " << payee_before.available << " -> "
                  << balance(*runtime, seller_account, instrument.quote_asset)
                         .available
                  << "\nLedger settlement: sequence=" << settlement.sequence
                  << " contract_id=" << settlement_metadata.contract_id
                  << " payer_account=" << settlement_metadata.payer_account_id
                  << " payee_account=" << settlement_metadata.payee_account_id
                  << "\nWAL: records=" << scan_wal(wal_path).records.size()
                  << "\n\n";

        runtime.reset();

        std::cout << "=== Stage 4: Durable Command, Ambiguous Response, Crash ===\n"
                  << "Application submits request_id=" << ambiguous_request_id
                  << " account=" << seller_account
                  << " SELL price=110 quantity=2\n";
        execute_ambiguous_request_then_crash(wal_path);
        const exchange::WalScanResult after_crash = scan_wal(wal_path);
        require(after_crash.records.size() == 7, "crash command is not durable");
        std::cout << "Durable executor completed the command.\n"
                  << "Business response delivered: false\n"
                  << "Process terminated with SIGKILL.\n"
                  << "WAL after crash: records=" << after_crash.records.size()
                  << " last_sequence=" << after_crash.records.back().sequence
                  << "\n\n";

        std::cout << "=== Stage 5: Restart and Exact Recovery ===\n";
        std::unique_ptr<exchange::TradingRuntime> recovered =
            exchange::TradingRuntime::create_durable(
                instrument, wal_path, initial_state);
        register_agents(*recovered);
        const auto recovered_order_1 = recovered->order_book().find_order(1);
        const auto recovered_order_3 = recovered->order_book().find_order(3);
        const auto recovered_reservation_1 = recovered->reservations().find(1);
        const auto recovered_reservation_3 = recovered->reservations().find(3);
        const auto recovered_contract = recovered->contracts().find(contract_id);
        require(
            recovered_order_1.has_value()
                && recovered_order_1->quantity == 1
                && recovered_order_3.has_value()
                && recovered_order_3->quantity == 2
                && recovered_reservation_1.has_value()
                && recovered_reservation_1->remaining_amount == 1
                && recovered_reservation_3.has_value()
                && recovered_reservation_3->remaining_amount == 2
                && recovered_contract.has_value()
                && recovered_contract->state
                    == exchange::ContractState::Settled
                && recovered->order_book().order_count() == 2
                && recovered->ledger().entries().size() == 5
                && balance(*recovered, buyer_account, instrument.base_asset)
                    == exchange::Balance{1, 0}
                && balance(*recovered, buyer_account, instrument.quote_asset)
                    == exchange::Balance{1'600, 0}
                && balance(*recovered, seller_account, instrument.base_asset)
                    == exchange::Balance{6, 3}
                && balance(*recovered, seller_account, instrument.quote_asset)
                    == exchange::Balance{500, 0},
            "recovered authoritative state does not match expected state");
        std::cout << "Recovered orders: id=1 remaining=1, id=3 remaining=2\n"
                  << "Recovered reservations: order=1 amount=1, order=3 amount=2\n";
        print_account("Recovered buyer", *recovered, buyer_account);
        print_account("Recovered seller", *recovered, seller_account);
        std::cout << "Recovered contract: id=" << contract_id
                  << " state="
                  << contract_state_name(recovered_contract->state)
                  << " resource_fulfilled="
                  << recovered_contract->resource_delivery_obligation.fulfilled
                  << " payment_fulfilled="
                  << recovered_contract->payment_obligation.fulfilled
                  << "\nRecovered Ledger: entries="
                  << recovered->ledger().entries().size()
                  << " last_sequence="
                  << recovered->ledger().entries().back().sequence
                  << "\nRecovered WAL: records="
                  << scan_wal(wal_path).records.size()
                  << "\nExact expected state recovered: true\n"
                  << "Source of truth: bootstrap + WAL; no Agent memory restored.\n\n";

        std::cout << "=== Stage 6: Explicit Retry Semantics ===\n";
        const exchange::TradingResponse repeated =
            recovered->executor().execute(exchange::TradingRequest{
                ambiguous_request_id,
                seller_account,
                exchange::SubmitTradingRequest{
                    exchange::Side::Sell,
                    110,
                    2}});
        require(
            repeated.result == exchange::TradingResult::Accepted
                && repeated.assigned_order_id == 4,
            "repeated RequestId was not treated as a new attempt");
        std::cout << "Repeated request_id=" << ambiguous_request_id
                  << " result=Accepted new_order_id="
                  << *repeated.assigned_order_id << "\n"
                  << "RequestId semantics: correlation only; no deduplication.\n";

        exchange::TradingRequestAgentExecutionAdapter recovered_execution{
            recovered->agent_registry(),
            recovered->executor(),
            recovered->contract_executor()};
        const exchange::Balance payer_retry_before = balance(
            *recovered, buyer_account, instrument.quote_asset);
        const exchange::Balance payee_retry_before = balance(
            *recovered, seller_account, instrument.quote_asset);
        const std::size_t ledger_retry_before =
            recovered->ledger().entries().size();
        const std::size_t wal_retry_before = scan_wal(wal_path).records.size();
        const exchange::AgentActionResult settlement_retry =
            recovered_execution.execute(
                buyer_agent,
                exchange::SettlePaymentObligationAction{contract_id});
        const auto& settlement_retry_result =
            std::get<exchange::ContractActionResult>(settlement_retry);
        require(
            settlement_retry_result.status
                    == exchange::ContractResult::InvalidTransition
                && balance(
                       *recovered,
                       buyer_account,
                       instrument.quote_asset)
                    == payer_retry_before
                && balance(
                       *recovered,
                       seller_account,
                       instrument.quote_asset)
                    == payee_retry_before
                && recovered->ledger().entries().size()
                    == ledger_retry_before
                && scan_wal(wal_path).records.size() == wal_retry_before,
            "settled contract retry changed authoritative payment state");
        std::cout << "Settlement retry: contract_id=" << contract_id
                  << " result="
                  << contract_result_name(settlement_retry_result.status)
                  << "\nDouble payment prevented: true"
                  << " (balances, Ledger, and WAL unchanged)\n\n";

        std::cout << "Demo complete: deterministic validation, internal "
                     "execution, synchronous durability, crash recovery, and "
                     "explicit retry semantics verified.\n";
    }
}  // namespace

int main() {
    try {
        run_demo();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "exchange_runtime_demo error: " << error.what() << '\n';
        return 1;
    }
}
