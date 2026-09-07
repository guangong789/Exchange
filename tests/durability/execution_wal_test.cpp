#include "exchange/durability/execution_wal.hpp"

#include "exchange/accounting/execution_coordinator.hpp"
#include "exchange/execution/execution_command_applier.hpp"
#include "exchange/execution/execution_sequencer.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <variant>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
        constexpr BootstrapFingerprint fingerprint{
            0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

        ExecutionCommand submit_command(
            Side side = Side::Buy,
            RequestId request_id = 11,
            AccountId account_id = 12,
            OrderId order_id = 13,
            Timestamp timestamp = 14,
            Price price = 100,
            Quantity quantity = 3) {
            return SubmitExecutionCommand{
                request_id,
                account_id,
                Order{
                    order_id,
                    side,
                    OrderType::Limit,
                    price,
                    quantity,
                    timestamp}};
        }

        ExecutionCommand cancel_command(
            RequestId request_id = 21,
            AccountId account_id = 22,
            OrderId order_id = 23) {
            return CancelExecutionCommand{
                request_id,
                account_id,
                order_id};
        }

        WalBytes encoded_header(
            const InstrumentContext& context = instrument) {
            WalHeaderEncodeResult result =
                encode_wal_file_header(context, fingerprint);
            EXPECT_TRUE(std::holds_alternative<WalBytes>(result));
            return std::get<WalBytes>(std::move(result));
        }

        WalBytes encoded_record(const WalRecord& record) {
            WalRecordEncodeResult result =
                encode_wal_record(record, instrument);
            EXPECT_TRUE(std::holds_alternative<WalBytes>(result));
            return std::get<WalBytes>(std::move(result));
        }

        void append(WalBytes& destination, const WalBytes& source) {
            destination.insert(
                destination.end(),
                source.begin(),
                source.end());
        }

        std::uint32_t test_crc32c(std::span<const std::uint8_t> bytes) {
            constexpr std::uint32_t polynomial = 0x82F63B78U;
            std::uint32_t crc = std::numeric_limits<std::uint32_t>::max();
            for (const std::uint8_t byte : bytes) {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit) {
                    const std::uint32_t mask =
                        0U - static_cast<std::uint32_t>(crc & 1U);
                    crc = (crc >> 1U) ^ (polynomial & mask);
                }
            }
            return ~crc;
        }

        void write_u16(WalBytes& bytes, std::size_t offset, std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
        }

        void write_u32(WalBytes& bytes, std::size_t offset, std::uint32_t value) {
            for (std::size_t index = 0; index < 4; ++index) {
                bytes[offset + index] = static_cast<std::uint8_t>(
                    value >> (index * 8));
            }
        }

        void rewrite_checksum(WalBytes& bytes) {
            const std::size_t checksum_offset = bytes.size() - 4;
            write_u32(
                bytes,
                checksum_offset,
                test_crc32c(std::span<const std::uint8_t>{bytes}.first(
                    checksum_offset)));
        }

        WalBytes file_with(std::initializer_list<WalRecord> records) {
            WalBytes bytes = encoded_header();
            for (const WalRecord& record : records) {
                append(bytes, encoded_record(record));
            }
            return bytes;
        }

        TEST(WalHeaderCodecTest, RoundTripPreservesCompleteInstrumentContext) {
            const WalBytes bytes = encoded_header();

            EXPECT_EQ(bytes.size(), kWalFileHeaderEncodedSize);
            EXPECT_EQ(bytes[8], 2U);
            EXPECT_EQ(bytes[9], 0U);
            EXPECT_EQ(bytes[10], kWalFileHeaderEncodedSize);
            EXPECT_EQ(bytes[11], 0U);
            const WalHeaderDecodeResult decoded =
                decode_wal_file_header(bytes);
            ASSERT_TRUE(std::holds_alternative<WalFileHeader>(decoded));
            EXPECT_EQ(std::get<WalFileHeader>(decoded).instrument, instrument);
            EXPECT_EQ(
                std::get<WalFileHeader>(decoded).bootstrap_fingerprint,
                fingerprint);
        }

        TEST(WalHeaderCodecTest, RejectsBadMagicVersionAndTruncation) {
            WalBytes bad_magic = encoded_header();
            bad_magic[0] ^= 0x01U;
            EXPECT_EQ(
                std::get<WalError>(decode_wal_file_header(bad_magic)),
                WalError::BadMagic);

            WalBytes bad_version = encoded_header();
            write_u16(bad_version, 8, 3);
            EXPECT_EQ(
                std::get<WalError>(decode_wal_file_header(bad_version)),
                WalError::UnsupportedFileVersion);

            WalBytes truncated = encoded_header();
            truncated.pop_back();
            EXPECT_EQ(
                std::get<WalError>(decode_wal_file_header(truncated)),
                WalError::TruncatedHeader);

            WalBytes bad_size = encoded_header();
            write_u16(bad_size, 10, 77);
            EXPECT_EQ(
                std::get<WalError>(decode_wal_file_header(bad_size)),
                WalError::InvalidHeaderSize);
        }

        TEST(WalHeaderCodecTest, RejectsPreviousVersionExplicitly) {
            WalBytes previous = encoded_header();
            previous.resize(44);
            write_u16(previous, 8, 1);
            write_u16(previous, 10, 44);

            EXPECT_EQ(
                std::get<WalError>(decode_wal_file_header(previous)),
                WalError::UnsupportedFileVersion);
        }

        TEST(WalScannerTest, DistinguishesEmptyAndTruncatedFiles) {
            EXPECT_EQ(
                scan_execution_wal({}, instrument, fingerprint).error,
                WalError::EmptyFile);
            const WalBytes partial{'E', 'X', 'W'};
            EXPECT_EQ(
                scan_execution_wal(partial, instrument, fingerprint).error,
                WalError::TruncatedHeader);
        }

        TEST(WalScannerTest, RejectsConfigurationMismatch) {
            const InstrumentContext other{20, 10, 2, 1, 1};
            const WalScanResult result = scan_execution_wal(
                encoded_header(),
                other,
                fingerprint);

            EXPECT_EQ(result.status, WalScanStatus::Error);
            EXPECT_EQ(result.error, WalError::ConfigurationMismatch);
            EXPECT_EQ(result.last_valid_offset, 0U);
        }

        TEST(WalScannerTest, RejectsChangedBootstrapFingerprint) {
            WalBytes corrupted = encoded_header();
            corrupted[44] ^= 0x80U;

            const WalScanResult result = scan_execution_wal(
                corrupted, instrument, fingerprint);

            EXPECT_EQ(result.status, WalScanStatus::Error);
            EXPECT_EQ(result.error, WalError::BootstrapMismatch);
            EXPECT_EQ(result.last_valid_offset, 0U);
        }

        TEST(WalRecordCodecTest, SubmitRoundTripPreservesBuyAndSellFields) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                const WalRecord original{7, submit_command(side)};
                const WalBytes bytes = encoded_record(original);
                ASSERT_EQ(bytes.size(), kWalSubmitRecordEncodedSize);

                const WalRecordDecodeResult decoded =
                    decode_wal_record(bytes, instrument);
                ASSERT_TRUE(std::holds_alternative<WalRecord>(decoded));
                const WalRecord& record = std::get<WalRecord>(decoded);
                const SubmitExecutionCommand& submit =
                    std::get<SubmitExecutionCommand>(record.command);
                EXPECT_EQ(record.sequence, 7U);
                EXPECT_EQ(submit.request_id, 11U);
                EXPECT_EQ(submit.account_id, 12U);
                EXPECT_EQ(submit.order.id, 13U);
                EXPECT_EQ(submit.order.timestamp, 14);
                EXPECT_EQ(submit.order.side, side);
                EXPECT_EQ(submit.order.type, OrderType::Limit);
                EXPECT_EQ(submit.order.price, 100);
                EXPECT_EQ(submit.order.quantity, 3);
            }
        }

        TEST(WalRecordCodecTest, SubmitPreservesBoundaryIdentityValues) {
            const WalRecord original{
                std::numeric_limits<WalSequence>::max(),
                submit_command(
                    Side::Buy,
                    std::numeric_limits<RequestId>::max(),
                    std::numeric_limits<AccountId>::max(),
                    std::numeric_limits<OrderId>::max() - 1,
                    std::numeric_limits<Timestamp>::max() - 1,
                    1,
                    1)};

            const WalRecordDecodeResult decoded = decode_wal_record(
                encoded_record(original),
                instrument);
            ASSERT_TRUE(std::holds_alternative<WalRecord>(decoded));
            const WalRecord& record = std::get<WalRecord>(decoded);
            const auto& submit = std::get<SubmitExecutionCommand>(
                record.command);
            EXPECT_EQ(record.sequence, std::numeric_limits<WalSequence>::max());
            EXPECT_EQ(
                submit.request_id,
                std::numeric_limits<RequestId>::max());
            EXPECT_EQ(
                submit.account_id,
                std::numeric_limits<AccountId>::max());
            EXPECT_EQ(
                submit.order.id,
                std::numeric_limits<OrderId>::max() - 1);
            EXPECT_EQ(
                submit.order.timestamp,
                std::numeric_limits<Timestamp>::max() - 1);
        }

        TEST(WalRecordCodecTest, CancelRoundTripPreservesExactTarget) {
            const WalRecord original{9, cancel_command(31, 32, 33)};
            const WalBytes bytes = encoded_record(original);
            ASSERT_EQ(bytes.size(), kWalCancelRecordEncodedSize);

            const WalRecordDecodeResult decoded =
                decode_wal_record(bytes, instrument);
            ASSERT_TRUE(std::holds_alternative<WalRecord>(decoded));
            const WalRecord& record = std::get<WalRecord>(decoded);
            const auto& cancel = std::get<CancelExecutionCommand>(
                record.command);
            EXPECT_EQ(record.sequence, 9U);
            EXPECT_EQ(cancel.request_id, 31U);
            EXPECT_EQ(cancel.account_id, 32U);
            EXPECT_EQ(cancel.order_id, 33U);
        }

        TEST(WalRecordCodecTest, EncodingIsDeterministicAndCanonical) {
            const WalRecord record{1, submit_command()};
            const WalBytes first = encoded_record(record);
            const WalBytes second = encoded_record(record);
            EXPECT_EQ(first, second);

            const WalRecordDecodeResult decoded =
                decode_wal_record(first, instrument);
            ASSERT_TRUE(std::holds_alternative<WalRecord>(decoded));
            EXPECT_EQ(encoded_record(std::get<WalRecord>(decoded)), first);

            ASSERT_EQ(first.size(), kWalSubmitRecordEncodedSize);
            EXPECT_EQ(first[0], 69U);
            EXPECT_EQ(first[4], 1U);
            EXPECT_EQ(first[6], 1U);
            EXPECT_EQ(first[48], 0U);
            EXPECT_EQ(first[first.size() - 4], 0x86U);
            EXPECT_EQ(first[first.size() - 3], 0x4fU);
            EXPECT_EQ(first[first.size() - 2], 0x62U);
            EXPECT_EQ(first[first.size() - 1], 0xdfU);
        }

        TEST(WalScannerTest, ScansContiguousMixedRecordsToCleanEof) {
            const WalBytes bytes = file_with({
                WalRecord{1, submit_command()},
                WalRecord{2, cancel_command()}});

            const WalScanResult result = scan_execution_wal(
                bytes, instrument, fingerprint);

            EXPECT_EQ(result.status, WalScanStatus::CleanEof);
            EXPECT_EQ(result.error, WalError::None);
            ASSERT_EQ(result.records.size(), 2U);
            EXPECT_EQ(result.records[0].sequence, 1U);
            EXPECT_EQ(result.records[1].sequence, 2U);
            EXPECT_EQ(result.last_valid_offset, bytes.size());
        }

        TEST(WalScannerTest, RejectsDuplicateGapOutOfOrderAndNonOneStart) {
            for (const auto& sequences : {
                     std::vector<WalSequence>{1, 1},
                     std::vector<WalSequence>{1, 3},
                     std::vector<WalSequence>{1, 3, 2},
                     std::vector<WalSequence>{2}}) {
                WalBytes bytes = encoded_header();
                for (const WalSequence sequence : sequences) {
                    append(bytes, encoded_record(
                        WalRecord{sequence, cancel_command()}));
                }

                const WalScanResult result =
                    scan_execution_wal(bytes, instrument, fingerprint);
                EXPECT_EQ(result.status, WalScanStatus::Error);
                EXPECT_EQ(result.error, WalError::SequenceMismatch);
            }
        }

        TEST(WalRecordCodecTest, DetectsChecksumCorruptionInEnvelopeAndPayload) {
            WalBytes envelope = encoded_record(
                WalRecord{1, submit_command()});
            envelope[8] ^= 0x20U;
            EXPECT_EQ(
                std::get<WalError>(decode_wal_record(envelope, instrument)),
                WalError::ChecksumMismatch);

            WalBytes payload = encoded_record(
                WalRecord{1, submit_command()});
            payload[49] ^= 0x01U;
            EXPECT_EQ(
                std::get<WalError>(decode_wal_record(payload, instrument)),
                WalError::ChecksumMismatch);
        }

        TEST(WalRecordCodecTest, RejectsUnknownTypeVersionAndFlags) {
            WalBytes unknown = encoded_record(
                WalRecord{1, cancel_command()});
            unknown[6] = 99;
            rewrite_checksum(unknown);
            EXPECT_EQ(
                std::get<WalError>(decode_wal_record(unknown, instrument)),
                WalError::UnknownRecordType);

            WalBytes version = encoded_record(
                WalRecord{1, cancel_command()});
            write_u16(version, 4, 2);
            rewrite_checksum(version);
            EXPECT_EQ(
                std::get<WalError>(decode_wal_record(version, instrument)),
                WalError::UnsupportedRecordVersion);

            WalBytes flags = encoded_record(
                WalRecord{1, cancel_command()});
            flags[7] = 1;
            rewrite_checksum(flags);
            EXPECT_EQ(
                std::get<WalError>(decode_wal_record(flags, instrument)),
                WalError::InvalidRecordFlags);
        }

        TEST(WalRecordCodecTest, RejectsMalformedExcessiveAndUnexpectedLengths) {
            WalBytes too_small = encoded_record(
                WalRecord{1, cancel_command()});
            write_u32(too_small, 0, 20);
            EXPECT_EQ(
                std::get<WalError>(decode_wal_record(too_small, instrument)),
                WalError::InvalidRecordLength);

            WalBytes excessive = encoded_record(
                WalRecord{1, cancel_command()});
            write_u32(
                excessive,
                0,
                static_cast<std::uint32_t>(kMaxWalRecordEncodedSize + 1));
            EXPECT_EQ(
                std::get<WalError>(decode_wal_record(excessive, instrument)),
                WalError::ExcessiveRecordLength);

            WalBytes extra = encoded_record(WalRecord{1, cancel_command()});
            extra.insert(extra.end() - 4, 0);
            write_u32(extra, 0, static_cast<std::uint32_t>(extra.size()));
            rewrite_checksum(extra);
            EXPECT_EQ(
                std::get<WalError>(decode_wal_record(extra, instrument)),
                WalError::UnexpectedPayloadSize);
        }

        TEST(WalRecordCodecTest, RejectsInvalidDurableCommandValues) {
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    WalRecord{0, submit_command()},
                    instrument)),
                WalError::InvalidValue);
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    WalRecord{1, cancel_command(0, 2, 3)},
                    instrument)),
                WalError::InvalidValue);
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    WalRecord{1, submit_command(
                        Side::Buy,
                        1,
                        1,
                        std::numeric_limits<OrderId>::max(),
                        1)},
                    instrument)),
                WalError::InvalidValue);
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    WalRecord{1, submit_command(
                        Side::Buy,
                        1,
                        1,
                        1,
                        std::numeric_limits<Timestamp>::max())},
                    instrument)),
                WalError::InvalidValue);

            WalBytes invalid_side = encoded_record(
                WalRecord{1, submit_command()});
            invalid_side[48] = 9;
            rewrite_checksum(invalid_side);
            EXPECT_EQ(
                std::get<WalError>(
                    decode_wal_record(invalid_side, instrument)),
                WalError::InvalidValue);

            WalBytes zero_request = encoded_record(
                WalRecord{1, cancel_command()});
            std::fill(zero_request.begin() + 16, zero_request.begin() + 24, 0);
            rewrite_checksum(zero_request);
            EXPECT_EQ(
                std::get<WalError>(
                    decode_wal_record(zero_request, instrument)),
                WalError::InvalidValue);
        }

        TEST(WalRecordCodecTest, RejectsNonExactAndOverflowingFinancialValues) {
            constexpr InstrumentContext rational{20, 10, 2, 1, 2};
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    WalRecord{1, submit_command(
                        Side::Buy, 1, 1, 1, 1, 3, 2)},
                    rational)),
                WalError::InvalidValue);
            EXPECT_EQ(
                std::get<WalError>(encode_wal_record(
                    WalRecord{1, submit_command(
                        Side::Sell,
                        1,
                        1,
                        1,
                        1,
                        4,
                        std::numeric_limits<Quantity>::max())},
                    rational)),
                WalError::InvalidValue);
        }

        TEST(WalScannerTest, EveryIncompleteFinalRecordBoundaryIsTornTail) {
            const WalBytes first = encoded_record(
                WalRecord{1, submit_command()});
            const WalBytes second = encoded_record(
                WalRecord{2, cancel_command()});
            const std::size_t valid_offset =
                kWalFileHeaderEncodedSize + first.size();

            for (std::size_t partial_size = 1;
                 partial_size < second.size();
                 ++partial_size) {
                WalBytes bytes = encoded_header();
                append(bytes, first);
                bytes.insert(
                    bytes.end(),
                    second.begin(),
                    second.begin() + partial_size);

                const WalScanResult result =
                    scan_execution_wal(bytes, instrument, fingerprint);
                ASSERT_EQ(result.status, WalScanStatus::TornTail)
                    << "partial size " << partial_size;
                EXPECT_EQ(result.error, WalError::TornFinalRecord);
                EXPECT_EQ(result.records.size(), 1U);
                EXPECT_EQ(result.last_valid_offset, valid_offset);
            }
        }

        TEST(WalScannerTest, CompleteChecksumFailureIsHardCorruption) {
            WalBytes record = encoded_record(
                WalRecord{1, submit_command()});
            record.back() ^= 0x80U;
            WalBytes bytes = encoded_header();
            append(bytes, record);

            const WalScanResult result = scan_execution_wal(
                bytes, instrument, fingerprint);

            EXPECT_EQ(result.status, WalScanStatus::Error);
            EXPECT_EQ(result.error, WalError::ChecksumMismatch);
            EXPECT_TRUE(result.records.empty());
            EXPECT_EQ(result.last_valid_offset, kWalFileHeaderEncodedSize);
        }

        TEST(WalExecutionIntegrationTest,
             DecodedCommandAppliesExactIdentityWithoutSequencerAllocation) {
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator coordinator{
                instrument,
                accounts,
                reservations,
                matching_engine,
                events,
                ledger};
            ExecutionCommandApplier applier{coordinator, events};
            ExecutionSequencer sequencer;
            ASSERT_TRUE(accounts.create_account(12));
            accounts.fund(12, 10, 1'000);

            const WalRecordDecodeResult decoded = decode_wal_record(
                encoded_record(WalRecord{1, submit_command()}),
                instrument);
            ASSERT_TRUE(std::holds_alternative<WalRecord>(decoded));
            const TradingResponse response = applier.apply(
                std::get<WalRecord>(decoded).command);

            EXPECT_EQ(response.result, TradingResult::Accepted);
            EXPECT_EQ(response.assigned_order_id, 13U);
            EXPECT_TRUE(matching_engine.order_book().find_order(13).has_value());
            EXPECT_EQ(
                sequencer.allocate(),
                (AssignedOrderIdentity{1, 1}));
        }
    }  // namespace
}  // namespace exchange
