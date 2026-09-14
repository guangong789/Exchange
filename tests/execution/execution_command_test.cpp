#include "execution/trading_command_applier.hpp"
#include "execution/trading_request_admission.hpp"
#include "execution/trading_request_executor.hpp"

#include <limits>
#include <stdexcept>
#include <variant>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext test_instrument{20, 10, 1, 1, 1};

        TradingRequest submit_request(
            RequestId request_id,
            AccountId account_id,
            Side side,
            Price price,
            Quantity quantity) {
            return TradingRequest{
                request_id,
                account_id,
                SubmitTradingRequest{side, price, quantity}};
        }

        struct ExecutionWorld {
            explicit ExecutionWorld(
                InstrumentContext context = test_instrument)
                : instrument(context),
                  coordinator(
                      instrument,
                      accounts,
                      reservations,
                      matching_engine,
                      events,
                      ledger),
                  applier(coordinator, events),
                  executor(
                      instrument,
                      coordinator,
                      events,
                      sequencer) {}

            const InstrumentContext instrument;
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator coordinator;
            ExecutionSequencer sequencer;
            TradingCommandApplier applier;
            TradingRequestExecutor executor;
        };

        TEST(ExecutionCommandAdmissionTest,
             SubmitBuildsExplicitDeterministicAccountAwareCommand) {
            ExecutionSequencer sequencer;

            const ExecutionAdmissionResult admission =
                admit_trading_request(
                    submit_request(41, 7, Side::Sell, 105, 3),
                    test_instrument,
                    sequencer);

            ASSERT_TRUE(std::holds_alternative<ExecutionCommand>(admission));
            const ExecutionCommand& command =
                std::get<ExecutionCommand>(admission);
            ASSERT_TRUE(std::holds_alternative<SubmitExecutionCommand>(
                command));
            const auto& submit = std::get<SubmitExecutionCommand>(command);
            EXPECT_EQ(submit.request_id, 41U);
            EXPECT_EQ(submit.account_id, 7U);
            EXPECT_EQ(submit.order.id, 1U);
            EXPECT_EQ(submit.order.timestamp, 1);
            EXPECT_EQ(submit.order.side, Side::Sell);
            EXPECT_EQ(submit.order.type, OrderType::Limit);
            EXPECT_EQ(submit.order.price, 105);
            EXPECT_EQ(submit.order.quantity, 3);

            EXPECT_EQ(
                sequencer.allocate(),
                (AssignedOrderIdentity{2, 2}));
        }

        TEST(ExecutionCommandAdmissionTest,
             CancelPreservesRequestAccountAndTargetWithoutSequencing) {
            ExecutionSequencer sequencer;
            const TradingRequest request{
                52,
                9,
                CancelTradingRequest{77}};

            const ExecutionAdmissionResult admission =
                admit_trading_request(request, test_instrument, sequencer);

            ASSERT_TRUE(std::holds_alternative<ExecutionCommand>(admission));
            const ExecutionCommand& command =
                std::get<ExecutionCommand>(admission);
            ASSERT_TRUE(std::holds_alternative<CancelExecutionCommand>(
                command));
            const auto& cancel = std::get<CancelExecutionCommand>(command);
            EXPECT_EQ(cancel.request_id, 52U);
            EXPECT_EQ(cancel.account_id, 9U);
            EXPECT_EQ(cancel.order_id, 77U);
            EXPECT_EQ(
                sequencer.allocate(),
                (AssignedOrderIdentity{1, 1}));
        }

        TEST(ExecutionCommandAdmissionTest,
             ConversionAndArithmeticInvalidSubmitsDoNotConsumeSequence) {
            constexpr InstrumentContext rational_instrument{
                20,
                10,
                2,
                1,
                2};
            ExecutionSequencer sequencer;

            const ExecutionAdmissionResult non_exact =
                admit_trading_request(
                    submit_request(61, 1, Side::Buy, 3, 2),
                    rational_instrument,
                    sequencer);
            const ExecutionAdmissionResult overflow =
                admit_trading_request(
                    submit_request(
                        62,
                        1,
                        Side::Sell,
                        4,
                        std::numeric_limits<Quantity>::max()),
                    rational_instrument,
                    sequencer);
            const ExecutionAdmissionResult valid =
                admit_trading_request(
                    submit_request(63, 1, Side::Buy, 4, 2),
                    rational_instrument,
                    sequencer);

            EXPECT_EQ(
                std::get<TradingResult>(non_exact),
                TradingResult::InvalidOrder);
            EXPECT_EQ(
                std::get<TradingResult>(overflow),
                TradingResult::InvalidOrder);
            ASSERT_TRUE(std::holds_alternative<ExecutionCommand>(valid));
            const auto& submit = std::get<SubmitExecutionCommand>(
                std::get<ExecutionCommand>(valid));
            EXPECT_EQ(submit.order.id, 1U);
            EXPECT_EQ(submit.order.timestamp, 1);
        }

        TEST(ExecutionCommandAdmissionTest,
             InvalidMetadataDoesNotConstructCommandOrConsumeSequence) {
            ExecutionSequencer sequencer;

            const ExecutionAdmissionResult zero_request =
                admit_trading_request(
                    submit_request(0, 1, Side::Buy, 100, 1),
                    test_instrument,
                    sequencer);
            const ExecutionAdmissionResult zero_account =
                admit_trading_request(
                    submit_request(1, 0, Side::Buy, 100, 1),
                    test_instrument,
                    sequencer);

            EXPECT_EQ(
                std::get<TradingResult>(zero_request),
                TradingResult::InvalidRequest);
            EXPECT_EQ(
                std::get<TradingResult>(zero_account),
                TradingResult::InvalidRequest);
            EXPECT_EQ(
                sequencer.allocate(),
                (AssignedOrderIdentity{1, 1}));
        }

        TEST(ExecutionCommandAdmissionTest,
             InvalidSidePriceAndQuantityDoNotConsumeSequence) {
            ExecutionSequencer sequencer;

            for (const TradingRequest& request : {
                     submit_request(
                         1,
                         1,
                         static_cast<Side>(99),
                         100,
                         1),
                     submit_request(2, 1, Side::Buy, 0, 1),
                     submit_request(3, 1, Side::Buy, 100, 0)}) {
                const ExecutionAdmissionResult admission =
                    admit_trading_request(
                        request,
                        test_instrument,
                        sequencer);
                ASSERT_TRUE(std::holds_alternative<TradingResult>(
                    admission));
                EXPECT_EQ(
                    std::get<TradingResult>(admission),
                    TradingResult::InvalidOrder);
            }

            const ExecutionAdmissionResult valid = admit_trading_request(
                submit_request(4, 1, Side::Buy, 100, 1),
                test_instrument,
                sequencer);
            ASSERT_TRUE(std::holds_alternative<ExecutionCommand>(valid));
            EXPECT_EQ(
                std::get<SubmitExecutionCommand>(
                    std::get<ExecutionCommand>(valid)).order.id,
                1U);
        }

        TEST(TradingCommandApplierTest,
             ExplicitSubmitUsesRecordedIdentityAndDoesNotAdvanceSequencer) {
            ExecutionWorld world;
            ASSERT_TRUE(world.accounts.create_account(1));
            world.accounts.fund(1, 10, 1'000);
            const ExecutionCommand command = SubmitExecutionCommand{
                71,
                1,
                Order{42, Side::Buy, OrderType::Limit, 100, 2, 99}};

            const TradingResponse response = world.applier.apply(command);

            EXPECT_EQ(response.request_id, 71U);
            EXPECT_EQ(response.result, TradingResult::Accepted);
            EXPECT_EQ(response.assigned_order_id, 42U);
            const auto order = world.matching_engine.order_book().find_order(42);
            ASSERT_TRUE(order.has_value());
            EXPECT_EQ(order->timestamp, 99);
            EXPECT_EQ(
                world.sequencer.allocate(),
                (AssignedOrderIdentity{1, 1}));
        }

        TEST(TradingCommandApplierTest,
             SameExplicitCommandProducesEquivalentFreshState) {
            ExecutionWorld first;
            ExecutionWorld second;
            for (ExecutionWorld* world : {&first, &second}) {
                ASSERT_TRUE(world->accounts.create_account(1));
                world->accounts.fund(1, 10, 1'000);
            }
            const ExecutionCommand command = SubmitExecutionCommand{
                81,
                1,
                Order{8, Side::Buy, OrderType::Limit, 100, 3, 14}};

            const TradingResponse first_response = first.applier.apply(command);
            const TradingResponse second_response = second.applier.apply(command);

            EXPECT_EQ(first_response.result, second_response.result);
            EXPECT_EQ(
                first.accounts.find_balance(1, 10),
                second.accounts.find_balance(1, 10));
            EXPECT_EQ(
                first.reservations.find(8),
                second.reservations.find(8));
            const auto first_order =
                first.matching_engine.order_book().find_order(8);
            const auto second_order =
                second.matching_engine.order_book().find_order(8);
            ASSERT_TRUE(first_order.has_value());
            ASSERT_TRUE(second_order.has_value());
            EXPECT_EQ(
                first_order->timestamp,
                second_order->timestamp);
            EXPECT_EQ(first.ledger.entries(), second.ledger.entries());
        }

        TEST(TradingCommandApplierTest,
             ExplicitCancelUsesRecordedTarget) {
            ExecutionWorld world;
            ASSERT_TRUE(world.accounts.create_account(1));
            world.accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                world.applier.apply(ExecutionCommand{
                    SubmitExecutionCommand{
                        91,
                        1,
                        Order{
                            12,
                            Side::Buy,
                            OrderType::Limit,
                            100,
                            2,
                            5}}}).result,
                TradingResult::Accepted);

            const TradingResponse response = world.applier.apply(
                ExecutionCommand{CancelExecutionCommand{92, 1, 12}});

            EXPECT_EQ(response.request_id, 92U);
            EXPECT_EQ(response.result, TradingResult::Cancelled);
            EXPECT_FALSE(response.assigned_order_id.has_value());
            EXPECT_FALSE(world.reservations.find(12).has_value());
            EXPECT_FALSE(
                world.matching_engine.order_book().find_order(12).has_value());
            EXPECT_EQ(world.accounts.find_balance(1, 10), (Balance{1'000, 0}));
        }

        TEST(TradingCommandApplierTest,
             MapsBusinessRejectionsWithoutChangingExplicitIdentity) {
            ExecutionWorld world;
            const ExecutionCommand missing_account = SubmitExecutionCommand{
                101,
                77,
                Order{55, Side::Buy, OrderType::Limit, 100, 1, 88}};

            const TradingResponse response =
                world.applier.apply(missing_account);

            EXPECT_EQ(response.request_id, 101U);
            EXPECT_EQ(response.result, TradingResult::AccountNotFound);
            EXPECT_FALSE(response.assigned_order_id.has_value());
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
        }

        TEST(TradingCommandApplierTest,
             UnexpectedCoordinatorExceptionPropagates) {
            ExecutionWorld world;
            ASSERT_TRUE(world.accounts.create_account(1));
            world.accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                world.accounts.reserve(1, 10, 100),
                ReserveResult::Success);
            ASSERT_TRUE(world.reservations.create(44, 1, 10, 100));

            EXPECT_THROW(
                static_cast<void>(world.applier.apply(ExecutionCommand{
                    CancelExecutionCommand{111, 1, 44}})),
                std::logic_error);
        }

        TEST(TradingCommandApplierTest,
             InvalidArgumentFromExplicitApplicationPropagates) {
            ExecutionWorld world;
            ASSERT_TRUE(world.accounts.create_account(1));
            world.accounts.fund(1, 10, 1'000);

            EXPECT_THROW(
                static_cast<void>(world.applier.apply(ExecutionCommand{
                    SubmitExecutionCommand{
                        112,
                        1,
                        Order{
                            45,
                            Side::Buy,
                            OrderType::Limit,
                            0,
                            1,
                            9}}})),
                std::invalid_argument);
        }

        TEST(ExecutionCommandParityTest,
             LiveAdmissionAndExplicitApplicationProduceSameBusinessState) {
            ExecutionWorld live;
            ExecutionWorld explicit_world;
            for (ExecutionWorld* world : {&live, &explicit_world}) {
                ASSERT_TRUE(world->accounts.create_account(1));
                world->accounts.fund(1, 10, 1'000);
            }

            const TradingResponse live_response = live.executor.execute(
                submit_request(121, 1, Side::Buy, 100, 2));
            const TradingResponse explicit_response =
                explicit_world.applier.apply(ExecutionCommand{
                    SubmitExecutionCommand{
                        121,
                        1,
                        Order{
                            1,
                            Side::Buy,
                            OrderType::Limit,
                            100,
                            2,
                            1}}});

            EXPECT_EQ(live_response.result, explicit_response.result);
            EXPECT_EQ(
                live_response.assigned_order_id,
                explicit_response.assigned_order_id);
            EXPECT_EQ(
                live.accounts.find_balance(1, 10),
                explicit_world.accounts.find_balance(1, 10));
            EXPECT_EQ(
                live.reservations.find(1),
                explicit_world.reservations.find(1));
            const auto live_order =
                live.matching_engine.order_book().find_order(1);
            const auto explicit_order =
                explicit_world.matching_engine.order_book().find_order(1);
            ASSERT_TRUE(live_order.has_value());
            ASSERT_TRUE(explicit_order.has_value());
            EXPECT_EQ(
                live_order->timestamp,
                explicit_order->timestamp);
            EXPECT_EQ(live.ledger.entries(), explicit_world.ledger.entries());
        }
    }  // namespace
}  // namespace exchange
