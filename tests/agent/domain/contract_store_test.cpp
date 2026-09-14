#include "agent/domain/contract_store.hpp"

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr AgentId proposer = 101;
        constexpr AgentId counterparty = 202;
        constexpr AgentId third_party = 303;

        ContractTerms valid_terms() {
            return ContractTerms{
                proposer,
                counterparty,
                500,
                ResourceKind::ComputeCredit,
                20};
        }

        class ContractStoreTest : public ::testing::Test {
        protected:
            ContractId create_contract() {
                const ContractId id = next_id++;
                EXPECT_EQ(
                    store.create_contract(
                        id,
                        proposer,
                        counterparty,
                        valid_terms()),
                    ContractResult::Success);
                return id;
            }

            ContractStore store;
            ContractId next_id{1};
        };

        TEST_F(ContractStoreTest,
               CreatesUniqueContractsWithExplicitImmutableObligations) {
            const ContractId first_id = create_contract();
            const ContractId second_id = next_id++;
            const ContractResult second = store.create_contract(
                second_id,
                proposer,
                counterparty,
                valid_terms());

            ASSERT_EQ(second, ContractResult::Success);
            EXPECT_NE(first_id, second_id);
            EXPECT_EQ(store.size(), 2U);

            std::optional<Contract> first = store.find(first_id);
            ASSERT_TRUE(first.has_value());
            EXPECT_EQ(first->id, first_id);
            EXPECT_EQ(first->proposer, proposer);
            EXPECT_EQ(first->counterparty, counterparty);
            EXPECT_EQ(first->terms, valid_terms());
            EXPECT_EQ(
                first->payment_obligation,
                (PaymentObligation{
                    proposer,
                    counterparty,
                    500,
                    false}));
            EXPECT_EQ(
                first->resource_delivery_obligation,
                (ResourceDeliveryObligation{
                    counterparty,
                    proposer,
                    ResourceKind::ComputeCredit,
                    20,
                    false}));
            EXPECT_EQ(first->state, ContractState::Proposed);

            first->terms.quote_payment_amount = 1;
            first->state = ContractState::Settled;
            const std::optional<Contract> authoritative =
                store.find(first_id);
            ASSERT_TRUE(authoritative.has_value());
            EXPECT_EQ(authoritative->terms, valid_terms());
            EXPECT_EQ(authoritative->state, ContractState::Proposed);
        }

        TEST_F(ContractStoreTest, RejectsInvalidPartiesAndTerms) {
            ContractTerms terms = valid_terms();
            EXPECT_EQ(
                store.create_contract(1, proposer, proposer, terms),
                ContractResult::InvalidTerms);

            terms.quote_payment_amount = 0;
            EXPECT_EQ(
                store.create_contract(
                    1,
                    proposer,
                    counterparty,
                    terms),
                ContractResult::InvalidTerms);
            terms.quote_payment_amount = -1;
            EXPECT_EQ(
                store.create_contract(
                    1,
                    proposer,
                    counterparty,
                    terms),
                ContractResult::InvalidTerms);
            terms = valid_terms();
            terms.resource_quantity = 0;
            EXPECT_EQ(
                store.create_contract(
                    1,
                    proposer,
                    counterparty,
                    terms),
                ContractResult::InvalidTerms);
            terms.resource_quantity = -1;
            EXPECT_EQ(
                store.create_contract(
                    1,
                    proposer,
                    counterparty,
                    terms),
                ContractResult::InvalidTerms);
            terms = valid_terms();
            terms.resource = static_cast<ResourceKind>(255);
            EXPECT_EQ(
                store.create_contract(
                    1,
                    proposer,
                    counterparty,
                    terms),
                ContractResult::InvalidTerms);
            terms = valid_terms();
            terms.payee = third_party;
            EXPECT_EQ(
                store.create_contract(
                    1,
                    proposer,
                    counterparty,
                    terms),
                ContractResult::InvalidTerms);
            EXPECT_EQ(store.size(), 0U);

            EXPECT_EQ(create_contract(), 1U);
        }

        TEST_F(ContractStoreTest,
               RelevantQueryReturnsOneCopyPerRelatedContract) {
            const ContractId id = create_contract();

            std::vector<Contract> proposer_contracts =
                store.find_relevant(proposer);
            const std::vector<Contract> counterparty_contracts =
                store.find_relevant(counterparty);
            EXPECT_EQ(proposer_contracts.size(), 1U);
            EXPECT_EQ(counterparty_contracts.size(), 1U);
            EXPECT_TRUE(store.find_relevant(third_party).empty());
            ASSERT_EQ(proposer_contracts.front().id, id);

            proposer_contracts.front().state = ContractState::Settled;
            EXPECT_EQ(store.find(id)->state, ContractState::Proposed);
        }

        TEST_F(ContractStoreTest, CompletesAuthorizedHappyPath) {
            const ContractId id = create_contract();

            EXPECT_EQ(
                store.accept_contract(id, counterparty),
                ContractResult::Success);
            EXPECT_EQ(store.find(id)->state, ContractState::Accepted);
            EXPECT_EQ(
                store.mark_fulfilled(id, counterparty),
                ContractResult::Success);
            ASSERT_TRUE(store.find(id).has_value());
            EXPECT_EQ(store.find(id)->state, ContractState::Fulfilled);
            EXPECT_TRUE(
                store.find(id)->resource_delivery_obligation.fulfilled);
            EXPECT_FALSE(store.find(id)->payment_obligation.fulfilled);
            EXPECT_EQ(
                store.mark_settled(id, proposer),
                ContractResult::Success);

            const std::optional<Contract> settled = store.find(id);
            ASSERT_TRUE(settled.has_value());
            EXPECT_EQ(settled->state, ContractState::Settled);
            EXPECT_TRUE(settled->resource_delivery_obligation.fulfilled);
            EXPECT_TRUE(settled->payment_obligation.fulfilled);
        }

        TEST_F(ContractStoreTest, CounterpartyCanRejectProposal) {
            const ContractId id = create_contract();

            EXPECT_EQ(
                store.reject_contract(id, counterparty),
                ContractResult::Success);
            ASSERT_TRUE(store.find(id).has_value());
            EXPECT_EQ(store.find(id)->state, ContractState::Rejected);
            EXPECT_EQ(
                store.accept_contract(id, counterparty),
                ContractResult::InvalidTransition);
            EXPECT_EQ(
                store.mark_settled(id, proposer),
                ContractResult::InvalidTransition);
        }

        TEST_F(ContractStoreTest, RejectsInvalidAndRepeatedTransitions) {
            const ContractId proposed_id = create_contract();
            EXPECT_EQ(
                store.mark_fulfilled(proposed_id, counterparty),
                ContractResult::InvalidTransition);
            EXPECT_EQ(
                store.mark_settled(proposed_id, proposer),
                ContractResult::InvalidTransition);

            EXPECT_EQ(
                store.accept_contract(proposed_id, counterparty),
                ContractResult::Success);
            EXPECT_EQ(
                store.accept_contract(proposed_id, counterparty),
                ContractResult::InvalidTransition);
            EXPECT_EQ(
                store.mark_fulfilled(proposed_id, counterparty),
                ContractResult::Success);
            EXPECT_EQ(
                store.mark_settled(proposed_id, proposer),
                ContractResult::Success);
            EXPECT_EQ(
                store.mark_fulfilled(proposed_id, counterparty),
                ContractResult::InvalidTransition);
            EXPECT_EQ(
                store.mark_settled(proposed_id, proposer),
                ContractResult::InvalidTransition);
        }

        TEST_F(ContractStoreTest, EnforcesTransitionAuthorization) {
            const ContractId id = create_contract();

            EXPECT_EQ(
                store.accept_contract(id, proposer),
                ContractResult::UnauthorizedActor);
            EXPECT_EQ(
                store.accept_contract(id, third_party),
                ContractResult::UnauthorizedActor);
            EXPECT_EQ(
                store.reject_contract(id, third_party),
                ContractResult::UnauthorizedActor);
            EXPECT_EQ(
                store.accept_contract(id, counterparty),
                ContractResult::Success);
            EXPECT_EQ(
                store.mark_fulfilled(id, proposer),
                ContractResult::UnauthorizedActor);
            EXPECT_EQ(
                store.mark_fulfilled(id, third_party),
                ContractResult::UnauthorizedActor);
            EXPECT_EQ(
                store.mark_fulfilled(id, counterparty),
                ContractResult::Success);
            EXPECT_EQ(
                store.mark_settled(id, counterparty),
                ContractResult::UnauthorizedActor);
            EXPECT_EQ(
                store.mark_settled(id, proposer),
                ContractResult::Success);
        }

        TEST_F(ContractStoreTest, UnknownContractIsANormalDomainResult) {
            EXPECT_FALSE(store.find(999).has_value());
            EXPECT_EQ(
                store.accept_contract(999, counterparty),
                ContractResult::ContractNotFound);
            EXPECT_EQ(
                store.reject_contract(999, counterparty),
                ContractResult::ContractNotFound);
            EXPECT_EQ(
                store.mark_fulfilled(999, counterparty),
                ContractResult::ContractNotFound);
            EXPECT_EQ(
                store.mark_settled(999, proposer),
                ContractResult::ContractNotFound);
        }

    }  // namespace
}  // namespace exchange
