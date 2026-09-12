#include "durability/execution_wal_writer.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
        constexpr BootstrapFingerprint fingerprint{
            0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

        ExecutionCommand submit_command(
            RequestId request_id = 11,
            OrderId order_id = 13) {
            return SubmitExecutionCommand{
                request_id,
                12,
                Order{
                    order_id,
                    Side::Buy,
                    OrderType::Limit,
                    100,
                    3,
                    static_cast<Timestamp>(order_id)}};
        }

        ExecutionCommand cancel_command(RequestId request_id = 21) {
            return CancelExecutionCommand{request_id, 12, 13};
        }

        class TemporaryDirectory {
        public:
            TemporaryDirectory() {
                std::string pattern = "/tmp/exchange-wal-test-XXXXXX";
                path_ = ::mkdtemp(pattern.data());
                if (path_.empty()) {
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "mkdtemp");
                }
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

        WalBytes read_file(const std::string& path) {
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd == -1) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "open test WAL");
            }
            struct stat status {};
            if (::fstat(fd, &status) == -1) {
                const int saved_errno = errno;
                ::close(fd);
                throw std::system_error(
                    saved_errno,
                    std::generic_category(),
                    "stat test WAL");
            }
            WalBytes bytes(static_cast<std::size_t>(status.st_size));
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const ssize_t count = ::read(
                    fd,
                    bytes.data() + offset,
                    bytes.size() - offset);
                if (count > 0) {
                    offset += static_cast<std::size_t>(count);
                    continue;
                }
                if (count == -1 && errno == EINTR) {
                    continue;
                }
                const int saved_errno = count == -1 ? errno : EIO;
                ::close(fd);
                throw std::system_error(
                    saved_errno,
                    std::generic_category(),
                    "read test WAL");
            }
            ::close(fd);
            return bytes;
        }

        void append_raw(const std::string& path, const WalBytes& bytes) {
            const int fd = ::open(
                path.c_str(),
                O_WRONLY | O_APPEND | O_CLOEXEC);
            if (fd == -1) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "open test WAL append");
            }
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const ssize_t count = ::write(
                    fd,
                    bytes.data() + offset,
                    bytes.size() - offset);
                if (count > 0) {
                    offset += static_cast<std::size_t>(count);
                    continue;
                }
                if (count == -1 && errno == EINTR) {
                    continue;
                }
                const int saved_errno = count == -1 ? errno : EIO;
                ::close(fd);
                throw std::system_error(
                    saved_errno,
                    std::generic_category(),
                    "append test WAL");
            }
            ::close(fd);
        }

        TEST(ExecutionWalWriterTest,
             CreatesCanonicalHeaderAndAppendsContiguousRecords) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{path, instrument, fingerprint};
                EXPECT_EQ(writer.record_count(), 0U);
                EXPECT_EQ(writer.next_sequence(), 1U);

                writer.append(submit_command());
                writer.append(cancel_command());
                EXPECT_EQ(writer.record_count(), 2U);
                EXPECT_EQ(writer.next_sequence(), 3U);
            }

            const WalBytes bytes = read_file(path);
            const WalScanResult scan = scan_execution_wal(
                bytes, instrument, fingerprint);
            ASSERT_EQ(scan.status, WalScanStatus::CleanEof);
            ASSERT_EQ(scan.records.size(), 2U);
            EXPECT_EQ(scan.records[0].sequence, 1U);
            EXPECT_EQ(scan.records[1].sequence, 2U);

            const WalHeaderEncodeResult header =
                encode_wal_file_header(instrument, fingerprint);
            ASSERT_TRUE(std::holds_alternative<WalBytes>(header));
            EXPECT_TRUE(std::equal(
                std::get<WalBytes>(header).begin(),
                std::get<WalBytes>(header).end(),
                bytes.begin()));
            const WalRecordEncodeResult first = encode_wal_record(
                WalRecord{1, submit_command()},
                instrument);
            ASSERT_TRUE(std::holds_alternative<WalBytes>(first));
            EXPECT_TRUE(std::equal(
                std::get<WalBytes>(first).begin(),
                std::get<WalBytes>(first).end(),
                bytes.begin() + kWalFileHeaderEncodedSize));
        }

        TEST(ExecutionWalWriterTest, ReopensAndResumesNextSequence) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{path, instrument, fingerprint};
                writer.append(submit_command());
                writer.append(cancel_command());
            }
            {
                ExecutionWalWriter writer{path, instrument, fingerprint};
                EXPECT_EQ(writer.record_count(), 2U);
                EXPECT_EQ(writer.next_sequence(), 3U);
                writer.append(submit_command(31, 14));
            }

            const WalScanResult scan = scan_execution_wal(
                read_file(path),
                instrument,
                fingerprint);
            ASSERT_EQ(scan.status, WalScanStatus::CleanEof);
            ASSERT_EQ(scan.records.size(), 3U);
            EXPECT_EQ(scan.records.back().sequence, 3U);
        }

        TEST(ExecutionWalWriterTest, RejectsConfigurationMismatch) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            { ExecutionWalWriter writer{path, instrument, fingerprint}; }

            try {
                ExecutionWalWriter writer{
                    path,
                    InstrumentContext{20, 10, 2, 1, 1},
                    fingerprint};
                FAIL() << "configuration mismatch was accepted";
            } catch (const ExecutionWalWriterException& error) {
                EXPECT_EQ(error.failure(), ExecutionWalWriterFailure::Scan);
                EXPECT_EQ(error.wal_error(), WalError::ConfigurationMismatch);
            }
        }

        TEST(ExecutionWalWriterTest, RejectsBootstrapMismatch) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            { ExecutionWalWriter writer{path, instrument, fingerprint}; }
            BootstrapFingerprint changed = fingerprint;
            changed[7] ^= 0x40U;

            try {
                ExecutionWalWriter writer{path, instrument, changed};
                FAIL() << "bootstrap mismatch was accepted";
            } catch (const ExecutionWalWriterException& error) {
                EXPECT_EQ(error.failure(), ExecutionWalWriterFailure::Scan);
                EXPECT_EQ(error.wal_error(), WalError::BootstrapMismatch);
            }
        }

        TEST(ExecutionWalWriterTest, RejectsCompleteRecordCorruption) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{path, instrument, fingerprint};
                writer.append(submit_command());
            }
            const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
            ASSERT_NE(fd, -1);
            const std::uint8_t corrupted = 0xff;
            constexpr off_t record_payload_offset =
                static_cast<off_t>(kWalFileHeaderEncodedSize + 16);
            ASSERT_EQ(
                ::pwrite(fd, &corrupted, 1, record_payload_offset),
                1);
            ASSERT_EQ(::fdatasync(fd), 0);
            ASSERT_EQ(::close(fd), 0);

            try {
                ExecutionWalWriter writer{path, instrument, fingerprint};
                FAIL() << "corrupt WAL was accepted";
            } catch (const ExecutionWalWriterException& error) {
                EXPECT_EQ(error.failure(), ExecutionWalWriterFailure::Scan);
                EXPECT_EQ(error.wal_error(), WalError::ChecksumMismatch);
            }
        }

        TEST(ExecutionWalWriterTest, TruncatesTornTailBeforeAppending) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{path, instrument, fingerprint};
                writer.append(submit_command());
            }
            const WalRecordEncodeResult second = encode_wal_record(
                WalRecord{2, cancel_command()},
                instrument);
            ASSERT_TRUE(std::holds_alternative<WalBytes>(second));
            WalBytes partial = std::get<WalBytes>(second);
            partial.resize(partial.size() - 2);
            append_raw(path, partial);

            {
                ExecutionWalWriter writer{path, instrument, fingerprint};
                EXPECT_EQ(writer.record_count(), 1U);
                EXPECT_EQ(writer.next_sequence(), 2U);
                writer.append(cancel_command());
            }

            const WalScanResult scan = scan_execution_wal(
                read_file(path),
                instrument,
                fingerprint);
            EXPECT_EQ(scan.status, WalScanStatus::CleanEof);
            EXPECT_EQ(scan.records.size(), 2U);
            EXPECT_EQ(scan.last_valid_offset, read_file(path).size());
        }

        TEST(ExecutionWalWriterTest, RefusesConcurrentWriter) {
            TemporaryDirectory directory;
            ExecutionWalWriter first{
                directory.wal_path(), instrument, fingerprint};

            try {
                ExecutionWalWriter second{
                    directory.wal_path(), instrument, fingerprint};
                FAIL() << "second writer acquired the same WAL";
            } catch (const ExecutionWalWriterException& error) {
                EXPECT_EQ(error.failure(), ExecutionWalWriterFailure::Lock);
            }
        }

        TEST(ExecutionWalWriterTest, OpenFailureIsFatal) {
            TemporaryDirectory directory;
            EXPECT_THROW(
                static_cast<void>(ExecutionWalWriter{
                    directory.wal_path() + "/missing/execution.wal",
                    instrument,
                    fingerprint}),
                ExecutionWalWriterException);
        }
    }  // namespace
}  // namespace exchange
