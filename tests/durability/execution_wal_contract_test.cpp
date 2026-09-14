#include "durability/execution_wal.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
        constexpr BootstrapFingerprint fingerprint{};

        CreateContractExecutionCommand create_command(
            ContractId contract_id = 1,
            AgentId proposer = 101,
            AgentId counterparty = 202,
            Amount payment = 500,
            ResourceQuantity quantity = 20) {
            return CreateContractExecutionCommand{
                contract_id,
                proposer,
                counterparty,
                ContractTerms{
                    proposer,
                    counterparty,
                    payment,
                    ResourceKind::ComputeCredit,
                    quantity}};
        }

        WalBytes encoded(const WalRecord& record) {
            WalRecordEncodeResult result = encode_wal_record(
                record,
                instrument);
            if (!std::holds_alternative<WalBytes>(result)) {
                return {};
            }
            return std::get<WalBytes>(std::move(result));
        }

        std::uint32_t crc32c_without_trailer(const WalBytes& bytes) {
            constexpr std::uint32_t polynomial = 0x82F63B78U;
            std::uint32_t crc = std::numeric_limits<std::uint32_t>::max();
            for (std::size_t index = 0; index < bytes.size() - 4; ++index) {
                crc ^= bytes[index];
                for (int bit = 0; bit < 8; ++bit) {
                    const std::uint32_t mask =
                        0U - static_cast<std::uint32_t>(crc & 1U);
                    crc = (crc >> 1U) ^ (polynomial & mask);
                }
            }
            return ~crc;
        }

        void rewrite_checksum(WalBytes& bytes) {
            const std::uint32_t checksum = crc32c_without_trailer(bytes);
            for (std::size_t index = 0; index < 4; ++index) {
                bytes[bytes.size() - 4 + index] =
                    static_cast<std::uint8_t>(checksum >> (index * 8));
            }
        }

        WalBytes header() {
            WalHeaderEncodeResult result = encode_wal_file_header(
                instrument,
                fingerprint);
            return std::get<WalBytes>(std::move(result));
        }

        template <typename Command>
        void expect_transition_round_trip(
            WalSequence sequence,
            const Command& command) {
            const WalBytes bytes = encoded({sequence, command});
            ASSERT_EQ(
                bytes.size(),
                kWalContractTransitionRecordEncodedSize);
            const WalRecordDecodeResult decoded = decode_wal_record(
                bytes,
                instrument);
            ASSERT_TRUE(std::holds_alternative<WalRecord>(decoded));
            const WalRecord& record = std::get<WalRecord>(decoded);
            EXPECT_EQ(record.sequence, sequence);
            ASSERT_TRUE(std::holds_alternative<Command>(record.command));
            EXPECT_EQ(std::get<Command>(record.command), command);
        }

        TEST(WalContractRecordCodecTest, CreateRoundTripPreservesAllTerms) {
            const WalRecord original{7, create_command()};
            const WalBytes bytes = encoded(original);

            ASSERT_EQ(bytes.size(), kWalCreateContractRecordEncodedSize);
            const WalRecordDecodeResult decoded = decode_wal_record(
                bytes,
                instrument);
            ASSERT_TRUE(std::holds_alternative<WalRecord>(decoded));
            EXPECT_EQ(std::get<WalRecord>(decoded).sequence, 7U);
            ASSERT_TRUE(std::holds_alternative<
                        CreateContractExecutionCommand>(
                std::get<WalRecord>(decoded).command));
            EXPECT_EQ(
                std::get<CreateContractExecutionCommand>(
                    std::get<WalRecord>(decoded).command),
                std::get<CreateContractExecutionCommand>(original.command));
        }

        TEST(WalContractRecordCodecTest,
             EveryTransitionRoundTripsWithExactActor) {
            expect_transition_round_trip(
                1,
                AcceptContractExecutionCommand{9, 202});
            expect_transition_round_trip(
                2,
                RejectContractExecutionCommand{9, 202});
            expect_transition_round_trip(
                3,
                FulfillResourceObligationExecutionCommand{9, 202});

            const SettlePaymentObligationExecutionCommand settlement{
                9,
                101,
                11,
                22};
            const WalBytes settlement_bytes = encoded({4, settlement});
            ASSERT_EQ(
                settlement_bytes.size(),
                kWalSettlePaymentRecordEncodedSize);
            const WalRecordDecodeResult settlement_decoded =
                decode_wal_record(settlement_bytes, instrument);
            ASSERT_TRUE(std::holds_alternative<WalRecord>(
                settlement_decoded));
            const WalRecord& settlement_record =
                std::get<WalRecord>(settlement_decoded);
            EXPECT_EQ(settlement_record.sequence, 4U);
            ASSERT_TRUE(std::holds_alternative<
                        SettlePaymentObligationExecutionCommand>(
                settlement_record.command));
            EXPECT_EQ(
                std::get<SettlePaymentObligationExecutionCommand>(
                    settlement_record.command),
                settlement);
        }

        TEST(WalContractRecordCodecTest, PreservesMaximumValidValues) {
            constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
            const CreateContractExecutionCommand command = create_command(
                maximum - 1,
                maximum,
                maximum - 1,
                std::numeric_limits<Amount>::max(),
                std::numeric_limits<ResourceQuantity>::max());

            const WalRecordDecodeResult decoded = decode_wal_record(
                encoded({maximum, command}),
                instrument);
            ASSERT_TRUE(std::holds_alternative<WalRecord>(decoded));
            ASSERT_TRUE(std::holds_alternative<
                        CreateContractExecutionCommand>(
                std::get<WalRecord>(decoded).command));
            EXPECT_EQ(
                std::get<CreateContractExecutionCommand>(
                    std::get<WalRecord>(decoded).command),
                command);
        }

        TEST(WalContractRecordCodecTest, PreservesMinimumValidValues) {
            const CreateContractExecutionCommand command = create_command(
                1,
                1,
                2,
                1,
                1);

            const WalRecordDecodeResult decoded = decode_wal_record(
                encoded({1, command}),
                instrument);
            ASSERT_TRUE(std::holds_alternative<WalRecord>(decoded));
            EXPECT_EQ(
                std::get<CreateContractExecutionCommand>(
                    std::get<WalRecord>(decoded).command),
                command);
        }

        TEST(WalContractRecordCodecTest, RejectsInvalidContractValues) {
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    {1, create_command(0)},
                    instrument)),
                WalError::InvalidValue);
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    {1, create_command(1, 101, 101)},
                    instrument)),
                WalError::InvalidValue);
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    {1, create_command(1, 101, 202, 0)},
                    instrument)),
                WalError::InvalidValue);
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    {1, create_command(1, 101, 202, 1, 0)},
                    instrument)),
                WalError::InvalidValue);
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    {1, AcceptContractExecutionCommand{0, 202}},
                    instrument)),
                WalError::InvalidValue);
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    {1, AcceptContractExecutionCommand{1, 0}},
                    instrument)),
                WalError::InvalidValue);
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    {1, SettlePaymentObligationExecutionCommand{
                            1, 101, 0, 2}},
                    instrument)),
                WalError::InvalidValue);
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    {1, SettlePaymentObligationExecutionCommand{
                            1, 101, 1, 1}},
                    instrument)),
                WalError::InvalidValue);
        }

        TEST(WalContractRecordCodecTest,
             DetectsTruncationUnknownTypeAndChecksumCorruption) {
            const WalBytes valid = encoded({1, create_command()});
            WalBytes truncated = valid;
            truncated.pop_back();
            EXPECT_EQ(
                std::get<WalError>(decode_wal_record(
                    truncated,
                    instrument)),
                WalError::InvalidRecordLength);

            WalBytes unknown = valid;
            unknown[6] = 8;
            rewrite_checksum(unknown);
            EXPECT_EQ(
                std::get<WalError>(decode_wal_record(
                    unknown,
                    instrument)),
                WalError::UnknownRecordType);

            WalBytes corrupt = valid;
            corrupt[20] ^= 0x01U;
            EXPECT_EQ(
                std::get<WalError>(decode_wal_record(
                    corrupt,
                    instrument)),
                WalError::ChecksumMismatch);
        }

        TEST(WalContractRecordCodecTest,
             ScannerHandlesMixedTradingAndContractRecordsAndTornTail) {
            WalBytes bytes = header();
            const std::vector<WalRecord> records{
                {1, SubmitExecutionCommand{
                    1,
                    1,
                    Order{1, Side::Buy, OrderType::Limit, 100, 1, 1}}},
                {2, create_command()},
                {3, CancelExecutionCommand{2, 1, 1}},
                {4, AcceptContractExecutionCommand{1, 202}},
            };
            for (const WalRecord& record : records) {
                const WalBytes record_bytes = encoded(record);
                bytes.insert(
                    bytes.end(),
                    record_bytes.begin(),
                    record_bytes.end());
            }

            const WalScanResult clean = scan_execution_wal(
                bytes,
                instrument,
                fingerprint);
            ASSERT_EQ(clean.status, WalScanStatus::CleanEof);
            ASSERT_EQ(clean.records.size(), records.size());
            for (std::size_t index = 0; index < records.size(); ++index) {
                EXPECT_EQ(clean.records[index].sequence, index + 1);
                EXPECT_EQ(
                    clean.records[index].command.index(),
                    records[index].command.index());
            }

            bytes.push_back(1);
            const WalScanResult torn = scan_execution_wal(
                bytes,
                instrument,
                fingerprint);
            EXPECT_EQ(torn.status, WalScanStatus::TornTail);
            EXPECT_EQ(torn.error, WalError::TornFinalRecord);
            ASSERT_EQ(torn.records.size(), records.size());
            for (std::size_t index = 0; index < records.size(); ++index) {
                EXPECT_EQ(torn.records[index].sequence, index + 1);
                EXPECT_EQ(
                    torn.records[index].command.index(),
                    records[index].command.index());
            }
        }
    }  // namespace
}  // namespace exchange
