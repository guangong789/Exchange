#pragma once

#include <optional>

#include "agent/domain/agent_registry.hpp"
#include "agent/domain/contract_store.hpp"
#include "durability/command_journal.hpp"
#include "execution/contract_command_applier.hpp"
#include "execution/contract_sequencer.hpp"
#include "execution/execution_runtime_status.hpp"

namespace exchange {
    class TradingRuntime;

    struct CreateContractRequest {
        AgentId proposer{};
        AgentId counterparty{};
        ContractTerms terms;
    };

    struct ContractExecutionResponse {
        ContractResult result{ContractResult::InvalidTerms};
        std::optional<ContractId> contract_id;

        bool operator==(const ContractExecutionResponse&) const = default;
    };

    class ContractRequestExecutor {
    public:
        ContractRequestExecutor(
            const AgentRegistry& registry,
            ContractStore& store,
            ContractSequencer& sequencer,
            ContractCommandApplier& command_applier,
            ExecutionCommandJournal* command_journal = nullptr,
            ExecutionRuntimeStatus* runtime_status = nullptr) noexcept;

        ContractRequestExecutor(const ContractRequestExecutor&) = delete;
        ContractRequestExecutor& operator=(const ContractRequestExecutor&) = delete;
        ContractRequestExecutor(ContractRequestExecutor&&) = delete;
        ContractRequestExecutor& operator=(ContractRequestExecutor&&) = delete;

        [[nodiscard]] ContractExecutionResponse create_contract(
            const CreateContractRequest& request);
        [[nodiscard]] ContractResult accept_contract(
            ContractId contract_id,
            AgentId acting_agent);
        [[nodiscard]] ContractResult reject_contract(
            ContractId contract_id,
            AgentId acting_agent);
        [[nodiscard]] ContractResult fulfill_resource(
            ContractId contract_id,
            AgentId acting_agent);
        [[nodiscard]] ContractResult settle_payment(
            ContractId contract_id,
            AgentId acting_agent);

        [[nodiscard]] bool poisoned() const noexcept;
        [[nodiscard]] bool durable_processing_started() const noexcept;

    private:
        friend class TradingRuntime;

        void attach_command_journal(
            ExecutionCommandJournal& command_journal) noexcept;
        [[nodiscard]] ContractResult execute_transition(
            ContractResult admission,
            ExecutionCommand command);
        void ensure_usable() const;
        void mark_durable_processing_started() noexcept;
        void poison() noexcept;

        const AgentRegistry& registry_;
        ContractStore& store_;
        ContractSequencer& sequencer_;
        ContractCommandApplier& command_applier_;
        ExecutionCommandJournal* command_journal_{};
        ExecutionRuntimeStatus* runtime_status_{};
        bool poisoned_{};
        bool durable_processing_started_{};
    };
}  // namespace exchange
