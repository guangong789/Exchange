#include "exchange/durability/execution_wal_writer.hpp"

#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace exchange {
    namespace {
        std::string error_message(
            std::string message,
            const std::error_code& error) {
            if (error) {
                message += ": ";
                message += error.message();
            }
            return message;
        }

        [[noreturn]] void throw_system_failure(
            ExecutionWalWriterFailure failure,
            const char* operation) {
            const std::error_code error{errno, std::generic_category()};
            throw ExecutionWalWriterException{
                failure,
                error_message(operation, error),
                error};
        }

        std::string parent_directory(const std::string& path) {
            const std::size_t separator = path.find_last_of('/');
            if (separator == std::string::npos) {
                return ".";
            }
            if (separator == 0) {
                return "/";
            }
            return path.substr(0, separator);
        }

        void close_noexcept(int fd) noexcept {
            if (fd >= 0) {
                static_cast<void>(::close(fd));
            }
        }
    }  // namespace

    ExecutionWalWriterException::ExecutionWalWriterException(
        ExecutionWalWriterFailure failure,
        std::string message,
        std::error_code system_error,
        WalError wal_error)
        : std::runtime_error(std::move(message)),
          failure_(failure),
          system_error_(system_error),
          wal_error_(wal_error) {}

    ExecutionWalWriterFailure
    ExecutionWalWriterException::failure() const noexcept {
        return failure_;
    }

    const std::error_code&
    ExecutionWalWriterException::system_error() const noexcept {
        return system_error_;
    }

    WalError ExecutionWalWriterException::wal_error() const noexcept {
        return wal_error_;
    }

    ExecutionWalWriter::ExecutionWalWriter(
        std::string path,
        InstrumentContext instrument,
        BootstrapFingerprint bootstrap_fingerprint)
        : path_(std::move(path)),
          instrument_(instrument),
          bootstrap_fingerprint_(bootstrap_fingerprint) {
        if (path_.empty()) {
            throw ExecutionWalWriterException{
                ExecutionWalWriterFailure::Open,
                "WAL path must not be empty"};
        }
        try {
            initialize();
        } catch (...) {
            close_noexcept(fd_);
            fd_ = -1;
            throw;
        }
    }

    ExecutionWalWriter::~ExecutionWalWriter() {
        close_noexcept(fd_);
    }

    void ExecutionWalWriter::initialize() {
        fd_ = ::open(
            path_.c_str(),
            O_CREAT | O_EXCL | O_RDWR | O_APPEND | O_CLOEXEC,
            0644);
        if (fd_ < 0 && errno == EEXIST) {
            fd_ = ::open(
                path_.c_str(),
                O_RDWR | O_APPEND | O_CLOEXEC);
        }
        if (fd_ < 0) {
            throw_system_failure(
                ExecutionWalWriterFailure::Open,
                "open WAL");
        }
        if (::flock(fd_, LOCK_EX | LOCK_NB) == -1) {
            throw_system_failure(
                ExecutionWalWriterFailure::Lock,
                "lock WAL");
        }

        struct stat status {};
        if (::fstat(fd_, &status) == -1) {
            throw_system_failure(
                ExecutionWalWriterFailure::Status,
                "stat WAL");
        }
        if (status.st_size < 0
            || static_cast<std::uintmax_t>(status.st_size)
                > std::numeric_limits<std::size_t>::max()) {
            throw ExecutionWalWriterException{
                ExecutionWalWriterFailure::Status,
                "WAL size is not representable"};
        }

        const std::size_t size = static_cast<std::size_t>(status.st_size);
        if (size == 0) {
            initialize_empty_file();
        } else {
            initialize_existing_file(size);
        }
    }

    void ExecutionWalWriter::initialize_empty_file() {
        const WalHeaderEncodeResult encoded =
            encode_wal_file_header(instrument_, bootstrap_fingerprint_);
        if (const auto* error = std::get_if<WalError>(&encoded)) {
            throw ExecutionWalWriterException{
                ExecutionWalWriterFailure::HeaderEncode,
                "encode WAL header",
                {},
                *error};
        }
        write_all(
            std::get<WalBytes>(encoded),
            ExecutionWalWriterFailure::HeaderWrite,
            "write WAL header");
        sync_file(
            ExecutionWalWriterFailure::HeaderSync,
            "sync WAL header");
        sync_parent_directory();
    }

    void ExecutionWalWriter::initialize_existing_file(std::size_t size) {
        WalBytes bytes(size);
        std::size_t read_offset = 0;
        while (read_offset < bytes.size()) {
            const ssize_t count = ::pread(
                fd_,
                bytes.data() + read_offset,
                bytes.size() - read_offset,
                static_cast<off_t>(read_offset));
            if (count > 0) {
                read_offset += static_cast<std::size_t>(count);
                continue;
            }
            if (count == -1 && errno == EINTR) {
                continue;
            }
            if (count == 0) {
                throw ExecutionWalWriterException{
                    ExecutionWalWriterFailure::Read,
                    "unexpected EOF while reading WAL"};
            }
            throw_system_failure(
                ExecutionWalWriterFailure::Read,
                "read WAL");
        }

        const WalScanResult scan = scan_execution_wal(
            bytes,
            instrument_,
            bootstrap_fingerprint_);
        if (scan.status == WalScanStatus::Error) {
            throw ExecutionWalWriterException{
                ExecutionWalWriterFailure::Scan,
                "WAL validation failed",
                {},
                scan.error};
        }
        if (scan.status == WalScanStatus::TornTail) {
            while (::ftruncate(
                       fd_,
                       static_cast<off_t>(scan.last_valid_offset)) == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw_system_failure(
                    ExecutionWalWriterFailure::Truncate,
                    "truncate torn WAL tail");
            }
            sync_file(
                ExecutionWalWriterFailure::TruncateSync,
                "sync truncated WAL");
        }

        record_count_ = scan.records.size();
        recovered_records_ = scan.records;
        if (!recovered_records_.empty()) {
            const WalSequence last_sequence =
                recovered_records_.back().sequence;
            if (last_sequence == std::numeric_limits<WalSequence>::max()) {
                next_sequence_ = last_sequence;
                sequence_exhausted_ = true;
            } else {
                next_sequence_ = last_sequence + 1;
            }
        }
    }

    void ExecutionWalWriter::append(const ExecutionCommand& command) {
        if (poisoned_) {
            throw ExecutionWalWriterException{
                ExecutionWalWriterFailure::Poisoned,
                "WAL writer is poisoned"};
        }
        if (sequence_exhausted_) {
            poisoned_ = true;
            throw ExecutionWalWriterException{
                ExecutionWalWriterFailure::SequenceExhausted,
                "WAL sequence is exhausted"};
        }

        const WalSequence sequence = next_sequence_;
        if (next_sequence_ == std::numeric_limits<WalSequence>::max()) {
            sequence_exhausted_ = true;
        } else {
            ++next_sequence_;
        }
        const WalRecordEncodeResult encoded = encode_wal_record(
            WalRecord{sequence, command},
            instrument_);
        if (const auto* error = std::get_if<WalError>(&encoded)) {
            poisoned_ = true;
            throw ExecutionWalWriterException{
                ExecutionWalWriterFailure::RecordEncode,
                "encode WAL record",
                {},
                *error};
        }

        try {
            write_all(
                std::get<WalBytes>(encoded),
                ExecutionWalWriterFailure::AppendWrite,
                "append WAL record");
            sync_file(
                ExecutionWalWriterFailure::AppendSync,
                "sync WAL record");
        } catch (...) {
            poisoned_ = true;
            throw;
        }
        ++record_count_;
    }

    void ExecutionWalWriter::write_all(
        const WalBytes& bytes,
        ExecutionWalWriterFailure failure,
        const char* operation) {
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t count = ::write(
                fd_,
                bytes.data() + written,
                bytes.size() - written);
            if (count > 0) {
                written += static_cast<std::size_t>(count);
                continue;
            }
            if (count == -1 && errno == EINTR) {
                continue;
            }
            if (count == 0) {
                throw ExecutionWalWriterException{
                    failure,
                    std::string{operation} + " returned zero"};
            }
            throw_system_failure(failure, operation);
        }
    }

    void ExecutionWalWriter::sync_file(
        ExecutionWalWriterFailure failure,
        const char* operation) {
        while (::fdatasync(fd_) == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw_system_failure(failure, operation);
        }
    }

    void ExecutionWalWriter::sync_parent_directory() {
        const std::string directory = parent_directory(path_);
        const int directory_fd = ::open(
            directory.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd == -1) {
            throw_system_failure(
                ExecutionWalWriterFailure::DirectoryOpen,
                "open WAL parent directory");
        }
        while (::fsync(directory_fd) == -1) {
            if (errno == EINTR) {
                continue;
            }
            const int saved_errno = errno;
            close_noexcept(directory_fd);
            errno = saved_errno;
            throw_system_failure(
                ExecutionWalWriterFailure::DirectorySync,
                "sync WAL parent directory");
        }
        close_noexcept(directory_fd);
    }

    WalSequence ExecutionWalWriter::next_sequence() const noexcept {
        return next_sequence_;
    }

    std::size_t ExecutionWalWriter::record_count() const noexcept {
        return record_count_;
    }

    bool ExecutionWalWriter::poisoned() const noexcept {
        return poisoned_;
    }

    const std::string& ExecutionWalWriter::path() const noexcept {
        return path_;
    }

    std::vector<WalRecord> ExecutionWalWriter::take_recovered_records() {
        return std::move(recovered_records_);
    }
}  // namespace exchange
