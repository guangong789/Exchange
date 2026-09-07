#pragma once

#include "exchange/accounting/financial_conversion.hpp"
#include "exchange/durability/command_journal.hpp"
#include "exchange/durability/execution_wal.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace exchange {
    enum class ExecutionWalWriterFailure {
        Open,
        Lock,
        Status,
        Read,
        HeaderEncode,
        HeaderWrite,
        HeaderSync,
        DirectoryOpen,
        DirectorySync,
        Scan,
        Truncate,
        TruncateSync,
        RecordEncode,
        AppendWrite,
        AppendSync,
        SequenceExhausted,
        Poisoned,
    };

    class ExecutionWalWriterException : public std::runtime_error {
    public:
        ExecutionWalWriterException(
            ExecutionWalWriterFailure failure,
            std::string message,
            std::error_code system_error = {},
            WalError wal_error = WalError::None);

        [[nodiscard]] ExecutionWalWriterFailure failure() const noexcept;
        [[nodiscard]] const std::error_code& system_error() const noexcept;
        [[nodiscard]] WalError wal_error() const noexcept;

    private:
        ExecutionWalWriterFailure failure_;
        std::error_code system_error_;
        WalError wal_error_;
    };

    // One instance is owned and called by one serialized execution thread.
    // It intentionally contains no internal synchronization.
    class ExecutionWalWriter final : public ExecutionCommandJournal {
    public:
        ExecutionWalWriter(
            std::string path,
            InstrumentContext instrument,
            BootstrapFingerprint bootstrap_fingerprint);
        ~ExecutionWalWriter() override;

        ExecutionWalWriter(const ExecutionWalWriter&) = delete;
        ExecutionWalWriter& operator=(const ExecutionWalWriter&) = delete;
        ExecutionWalWriter(ExecutionWalWriter&&) = delete;
        ExecutionWalWriter& operator=(ExecutionWalWriter&&) = delete;

        void append(const ExecutionCommand& command) override;

        [[nodiscard]] WalSequence next_sequence() const noexcept;
        [[nodiscard]] std::size_t record_count() const noexcept;
        [[nodiscard]] bool poisoned() const noexcept;
        [[nodiscard]] const std::string& path() const noexcept;
        [[nodiscard]] std::vector<WalRecord> take_recovered_records();

    private:
        void initialize();
        void initialize_empty_file();
        void initialize_existing_file(std::size_t size);
        void write_all(
            const WalBytes& bytes,
            ExecutionWalWriterFailure failure,
            const char* operation);
        void sync_file(
            ExecutionWalWriterFailure failure,
            const char* operation);
        void sync_parent_directory();

        std::string path_;
        const InstrumentContext instrument_;
        const BootstrapFingerprint bootstrap_fingerprint_;
        int fd_{-1};
        WalSequence next_sequence_{1};
        std::size_t record_count_{};
        std::vector<WalRecord> recovered_records_;
        bool sequence_exhausted_{};
        bool poisoned_{};
    };
}  // namespace exchange
