#include "exchange/durability/execution_wal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace exchange {
    namespace {
        constexpr std::array<std::uint8_t, 8> file_magic{
            'E', 'X', 'W', 'A', 'L', '0', '0', '1'};
        constexpr std::uint16_t file_version = 2;
        constexpr std::uint16_t record_version = 1;
        constexpr std::uint8_t submit_record_type = 1;
        constexpr std::uint8_t cancel_record_type = 2;
        constexpr std::uint8_t record_flags = 0;
        constexpr std::size_t record_prefix_size = 16;
        constexpr std::size_t checksum_size = 4;

        template <typename Integer>
        using UnsignedInteger = std::make_unsigned_t<Integer>;

        template <typename Integer>
        void append_little_endian(WalBytes& bytes, Integer value) {
            using Unsigned = UnsignedInteger<Integer>;
            const Unsigned encoded = [&] {
                if constexpr (std::is_signed_v<Integer>) {
                    return std::bit_cast<Unsigned>(value);
                } else {
                    return value;
                }
            }();
            for (std::size_t index = 0; index < sizeof(Integer); ++index) {
                bytes.push_back(static_cast<std::uint8_t>(
                    encoded >> (index * 8)));
            }
        }

        template <typename Integer>
        Integer read_little_endian(
            std::span<const std::uint8_t> bytes,
            std::size_t& offset) {
            using Unsigned = UnsignedInteger<Integer>;
            Unsigned value{};
            for (std::size_t index = 0; index < sizeof(Integer); ++index) {
                value |= static_cast<Unsigned>(bytes[offset++])
                    << (index * 8);
            }
            if constexpr (std::is_signed_v<Integer>) {
                return std::bit_cast<Integer>(value);
            } else {
                return value;
            }
        }

        std::uint32_t crc32c(std::span<const std::uint8_t> bytes) {
            constexpr std::uint32_t reversed_castagnoli = 0x82F63B78U;
            std::uint32_t crc = std::numeric_limits<std::uint32_t>::max();
            for (const std::uint8_t byte : bytes) {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit) {
                    const std::uint32_t mask =
                        0U - static_cast<std::uint32_t>(crc & 1U);
                    crc = (crc >> 1U) ^ (reversed_castagnoli & mask);
                }
            }
            return ~crc;
        }

        bool is_valid_instrument(const InstrumentContext& instrument) {
            try {
                validate_instrument_context(instrument);
                return true;
            } catch (const std::invalid_argument&) {
                return false;
            }
        }

        bool is_valid_submit(
            const SubmitExecutionCommand& submit,
            const InstrumentContext& instrument) {
            if (submit.request_id == 0 || submit.account_id == 0
                || submit.order.id == 0 || submit.order.timestamp <= 0
                || submit.order.id == std::numeric_limits<OrderId>::max()
                || submit.order.timestamp
                    == std::numeric_limits<Timestamp>::max()
                || submit.order.type != OrderType::Limit
                || (submit.order.side != Side::Buy
                    && submit.order.side != Side::Sell)
                || submit.order.price <= 0 || submit.order.quantity <= 0) {
                return false;
            }

            try {
                static_cast<void>(calculate_order_reservation(
                    instrument,
                    submit.order.side,
                    submit.order.price,
                    submit.order.quantity));
                return true;
            } catch (const std::invalid_argument&) {
                return false;
            } catch (const std::overflow_error&) {
                return false;
            }
        }

        bool is_valid_cancel(const CancelExecutionCommand& cancel) {
            return cancel.request_id != 0 && cancel.account_id != 0
                && cancel.order_id != 0;
        }

        bool is_valid_command(
            const ExecutionCommand& command,
            const InstrumentContext& instrument) {
            return std::visit(
                [&instrument](const auto& payload) {
                    using Command = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<
                                      Command,
                                      SubmitExecutionCommand>) {
                        return is_valid_submit(payload, instrument);
                    } else {
                        return is_valid_cancel(payload);
                    }
                },
                command);
        }

        void append_record_prefix(
            WalBytes& bytes,
            std::uint32_t length,
            std::uint8_t type,
            WalSequence sequence) {
            append_little_endian(bytes, length);
            append_little_endian(bytes, record_version);
            append_little_endian(bytes, type);
            append_little_endian(bytes, record_flags);
            append_little_endian(bytes, sequence);
        }

        WalError validate_record_frame(
            std::span<const std::uint8_t> bytes,
            std::uint8_t& type) {
            if (bytes.size() < sizeof(std::uint32_t)) {
                return WalError::InvalidRecordLength;
            }

            std::size_t offset = 0;
            const std::uint32_t encoded_length =
                read_little_endian<std::uint32_t>(bytes, offset);
            if (encoded_length < kWalCancelRecordEncodedSize) {
                return WalError::InvalidRecordLength;
            }
            if (encoded_length > kMaxWalRecordEncodedSize) {
                return WalError::ExcessiveRecordLength;
            }
            if (encoded_length != bytes.size()) {
                return WalError::InvalidRecordLength;
            }

            const std::uint32_t expected_checksum = [&] {
                std::size_t checksum_offset = bytes.size() - checksum_size;
                return read_little_endian<std::uint32_t>(
                    bytes,
                    checksum_offset);
            }();
            if (crc32c(bytes.first(bytes.size() - checksum_size))
                != expected_checksum) {
                return WalError::ChecksumMismatch;
            }

            offset = sizeof(std::uint32_t);
            if (read_little_endian<std::uint16_t>(bytes, offset)
                != record_version) {
                return WalError::UnsupportedRecordVersion;
            }
            type = read_little_endian<std::uint8_t>(bytes, offset);
            if (type != submit_record_type && type != cancel_record_type) {
                return WalError::UnknownRecordType;
            }
            if (read_little_endian<std::uint8_t>(bytes, offset)
                != record_flags) {
                return WalError::InvalidRecordFlags;
            }

            const std::size_t expected_size = type == submit_record_type
                ? kWalSubmitRecordEncodedSize
                : kWalCancelRecordEncodedSize;
            if (bytes.size() != expected_size) {
                return WalError::UnexpectedPayloadSize;
            }
            return WalError::None;
        }
    }  // namespace

    WalHeaderEncodeResult encode_wal_file_header(
        const InstrumentContext& instrument,
        const BootstrapFingerprint& bootstrap_fingerprint) {
        if (!is_valid_instrument(instrument)) {
            return WalError::InvalidInstrument;
        }

        WalBytes bytes;
        bytes.reserve(kWalFileHeaderEncodedSize);
        bytes.insert(bytes.end(), file_magic.begin(), file_magic.end());
        append_little_endian(bytes, file_version);
        append_little_endian(
            bytes,
            static_cast<std::uint16_t>(kWalFileHeaderEncodedSize));
        append_little_endian(bytes, instrument.base_asset);
        append_little_endian(bytes, instrument.quote_asset);
        append_little_endian(
            bytes,
            instrument.base_atomic_units_per_quantity_unit);
        append_little_endian(
            bytes,
            instrument.quote_atomic_units_per_price_quantity_numerator);
        append_little_endian(
            bytes,
            instrument.quote_atomic_units_per_price_quantity_denominator);
        bytes.insert(
            bytes.end(),
            bootstrap_fingerprint.begin(),
            bootstrap_fingerprint.end());
        return bytes;
    }

    WalHeaderDecodeResult decode_wal_file_header(
        std::span<const std::uint8_t> bytes) {
        constexpr std::size_t versioned_prefix_size = 12;
        if (bytes.size() < file_magic.size()) {
            return WalError::TruncatedHeader;
        }
        if (!std::equal(file_magic.begin(), file_magic.end(), bytes.begin())) {
            return WalError::BadMagic;
        }

        if (bytes.size() < versioned_prefix_size) {
            return WalError::TruncatedHeader;
        }

        std::size_t offset = file_magic.size();
        if (read_little_endian<std::uint16_t>(bytes, offset)
            != file_version) {
            return WalError::UnsupportedFileVersion;
        }
        if (read_little_endian<std::uint16_t>(bytes, offset)
            != kWalFileHeaderEncodedSize) {
            return WalError::InvalidHeaderSize;
        }
        if (bytes.size() < kWalFileHeaderEncodedSize) {
            return WalError::TruncatedHeader;
        }

        const InstrumentContext instrument{
            read_little_endian<AssetId>(bytes, offset),
            read_little_endian<AssetId>(bytes, offset),
            read_little_endian<Amount>(bytes, offset),
            read_little_endian<Amount>(bytes, offset),
            read_little_endian<Amount>(bytes, offset)};
        if (!is_valid_instrument(instrument)) {
            return WalError::InvalidInstrument;
        }
        BootstrapFingerprint bootstrap_fingerprint{};
        std::copy_n(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bootstrap_fingerprint.size(),
            bootstrap_fingerprint.begin());
        return WalFileHeader{instrument, bootstrap_fingerprint};
    }

    WalRecordEncodeResult encode_wal_record(
        const WalRecord& record,
        const InstrumentContext& instrument) {
        if (!is_valid_instrument(instrument) || record.sequence == 0
            || !is_valid_command(record.command, instrument)) {
            return WalError::InvalidValue;
        }

        WalBytes bytes;
        std::visit(
            [&bytes, &record](const auto& payload) {
                using Command = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<
                                  Command,
                                  SubmitExecutionCommand>) {
                    bytes.reserve(kWalSubmitRecordEncodedSize);
                    append_record_prefix(
                        bytes,
                        kWalSubmitRecordEncodedSize,
                        submit_record_type,
                        record.sequence);
                    append_little_endian(bytes, payload.request_id);
                    append_little_endian(bytes, payload.account_id);
                    append_little_endian(bytes, payload.order.id);
                    append_little_endian(bytes, payload.order.timestamp);
                    append_little_endian(
                        bytes,
                        static_cast<std::uint8_t>(payload.order.side));
                    append_little_endian(bytes, payload.order.price);
                    append_little_endian(bytes, payload.order.quantity);
                } else {
                    bytes.reserve(kWalCancelRecordEncodedSize);
                    append_record_prefix(
                        bytes,
                        kWalCancelRecordEncodedSize,
                        cancel_record_type,
                        record.sequence);
                    append_little_endian(bytes, payload.request_id);
                    append_little_endian(bytes, payload.account_id);
                    append_little_endian(bytes, payload.order_id);
                }
            },
            record.command);
        append_little_endian(bytes, crc32c(bytes));
        return bytes;
    }

    WalRecordDecodeResult decode_wal_record(
        std::span<const std::uint8_t> bytes,
        const InstrumentContext& instrument) {
        if (!is_valid_instrument(instrument)) {
            return WalError::InvalidInstrument;
        }

        std::uint8_t type{};
        const WalError frame_error = validate_record_frame(bytes, type);
        if (frame_error != WalError::None) {
            return frame_error;
        }

        std::size_t offset = sizeof(std::uint32_t)
            + sizeof(std::uint16_t) + sizeof(std::uint8_t)
            + sizeof(std::uint8_t);
        const WalSequence sequence =
            read_little_endian<WalSequence>(bytes, offset);
        if (sequence == 0) {
            return WalError::InvalidValue;
        }

        ExecutionCommand command = [&]() -> ExecutionCommand {
            const RequestId request_id =
                read_little_endian<RequestId>(bytes, offset);
            const AccountId account_id =
                read_little_endian<AccountId>(bytes, offset);
            if (type == submit_record_type) {
                const OrderId order_id =
                    read_little_endian<OrderId>(bytes, offset);
                const Timestamp timestamp =
                    read_little_endian<Timestamp>(bytes, offset);
                const Side side = static_cast<Side>(
                    read_little_endian<std::uint8_t>(bytes, offset));
                const Price price =
                    read_little_endian<Price>(bytes, offset);
                const Quantity quantity =
                    read_little_endian<Quantity>(bytes, offset);
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
            return CancelExecutionCommand{
                request_id,
                account_id,
                read_little_endian<OrderId>(bytes, offset)};
        }();
        if (!is_valid_command(command, instrument)) {
            return WalError::InvalidValue;
        }
        return WalRecord{sequence, command};
    }

    WalScanResult scan_execution_wal(
        std::span<const std::uint8_t> bytes,
        const InstrumentContext& expected_instrument,
        const BootstrapFingerprint& expected_bootstrap_fingerprint) {
        if (bytes.empty()) {
            return WalScanResult{
                WalScanStatus::Error,
                WalError::EmptyFile,
                {},
                0};
        }
        const WalHeaderDecodeResult decoded_header =
            decode_wal_file_header(bytes);
        if (const auto* error = std::get_if<WalError>(&decoded_header)) {
            return WalScanResult{
                WalScanStatus::Error,
                *error,
                {},
                0};
        }
        if (std::get<WalFileHeader>(decoded_header).instrument
            != expected_instrument) {
            return WalScanResult{
                WalScanStatus::Error,
                WalError::ConfigurationMismatch,
                {},
                0};
        }
        if (std::get<WalFileHeader>(decoded_header).bootstrap_fingerprint
            != expected_bootstrap_fingerprint) {
            return WalScanResult{
                WalScanStatus::Error,
                WalError::BootstrapMismatch,
                {},
                0};
        }

        WalScanResult result{
            WalScanStatus::CleanEof,
            WalError::None,
            {},
            kWalFileHeaderEncodedSize};
        std::size_t offset = kWalFileHeaderEncodedSize;
        WalSequence expected_sequence = 1;
        while (offset < bytes.size()) {
            const std::size_t remaining = bytes.size() - offset;
            if (remaining < sizeof(std::uint32_t)) {
                result.status = WalScanStatus::TornTail;
                result.error = WalError::TornFinalRecord;
                return result;
            }

            std::size_t length_offset = offset;
            const std::uint32_t record_length =
                read_little_endian<std::uint32_t>(bytes, length_offset);
            if (record_length < kWalCancelRecordEncodedSize) {
                result.status = WalScanStatus::Error;
                result.error = WalError::InvalidRecordLength;
                return result;
            }
            if (record_length > kMaxWalRecordEncodedSize) {
                result.status = WalScanStatus::Error;
                result.error = WalError::ExcessiveRecordLength;
                return result;
            }
            if (record_length > remaining) {
                result.status = WalScanStatus::TornTail;
                result.error = WalError::TornFinalRecord;
                return result;
            }

            const WalRecordDecodeResult decoded = decode_wal_record(
                bytes.subspan(offset, record_length),
                expected_instrument);
            if (const auto* error = std::get_if<WalError>(&decoded)) {
                result.status = WalScanStatus::Error;
                result.error = *error;
                return result;
            }
            const WalRecord& record = std::get<WalRecord>(decoded);
            if (record.sequence != expected_sequence) {
                result.status = WalScanStatus::Error;
                result.error = WalError::SequenceMismatch;
                return result;
            }
            result.records.push_back(record);
            offset += record_length;
            result.last_valid_offset = offset;
            ++expected_sequence;
        }
        return result;
    }
}  // namespace exchange
