#pragma once

#include "accounting/financial_conversion.hpp"
#include "execution/execution_command.hpp"
#include "execution/trading_bootstrap.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace exchange {
    using WalSequence = std::uint64_t;
    using WalBytes = std::vector<std::uint8_t>;

    inline constexpr std::size_t kWalFileHeaderEncodedSize = 76;
    inline constexpr std::size_t kWalSubmitRecordEncodedSize = 69;
    inline constexpr std::size_t kWalCancelRecordEncodedSize = 44;
    inline constexpr std::size_t kWalCreateContractRecordEncodedSize = 77;
    inline constexpr std::size_t kWalContractTransitionRecordEncodedSize = 36;
    inline constexpr std::size_t kWalSettlePaymentRecordEncodedSize = 52;
    inline constexpr std::size_t kMaxWalRecordEncodedSize = 4096;

    struct WalFileHeader {
        InstrumentContext instrument;
        BootstrapFingerprint bootstrap_fingerprint;

        bool operator==(const WalFileHeader&) const = default;
    };

    struct WalRecord {
        WalSequence sequence{};
        ExecutionCommand command;
    };

    enum class WalError {
        None,
        EmptyFile,
        TruncatedHeader,
        BadMagic,
        UnsupportedFileVersion,
        InvalidHeaderSize,
        InvalidInstrument,
        ConfigurationMismatch,
        BootstrapMismatch,
        InvalidRecordLength,
        ExcessiveRecordLength,
        UnsupportedRecordVersion,
        UnknownRecordType,
        InvalidRecordFlags,
        ChecksumMismatch,
        SequenceMismatch,
        InvalidValue,
        UnexpectedPayloadSize,
        TornFinalRecord,
    };

    using WalHeaderEncodeResult = std::variant<WalBytes, WalError>;
    using WalHeaderDecodeResult = std::variant<WalFileHeader, WalError>;
    using WalRecordEncodeResult = std::variant<WalBytes, WalError>;
    using WalRecordDecodeResult = std::variant<WalRecord, WalError>;

    enum class WalScanStatus {
        CleanEof,
        TornTail,
        Error,
    };

    struct WalScanResult {
        WalScanStatus status{WalScanStatus::Error};
        WalError error{WalError::None};
        std::vector<WalRecord> records;
        std::size_t last_valid_offset{};
    };

    // All integers are encoded explicitly in little-endian byte order.
    [[nodiscard]] WalHeaderEncodeResult encode_wal_file_header(
        const InstrumentContext& instrument,
        const BootstrapFingerprint& bootstrap_fingerprint);

    [[nodiscard]] WalHeaderDecodeResult decode_wal_file_header(
        std::span<const std::uint8_t> bytes);

    [[nodiscard]] WalRecordEncodeResult encode_wal_record(
        const WalRecord& record,
        const InstrumentContext& instrument);

    [[nodiscard]] WalRecordDecodeResult decode_wal_record(
        std::span<const std::uint8_t> bytes,
        const InstrumentContext& instrument);

    [[nodiscard]] WalScanResult scan_execution_wal(
        std::span<const std::uint8_t> bytes,
        const InstrumentContext& expected_instrument,
        const BootstrapFingerprint& expected_bootstrap_fingerprint);
}  // namespace exchange
