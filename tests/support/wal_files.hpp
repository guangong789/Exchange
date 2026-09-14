#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace exchange::test {
    class TempTestDirectory {
    public:
        TempTestDirectory() {
            std::string pattern = "/tmp/exchange-test-XXXXXX";
            const char* created = ::mkdtemp(pattern.data());
            if (created == nullptr) {
                throw std::system_error(errno, std::generic_category(), "mkdtemp");
            }
            path_ = created;
        }

        TempTestDirectory(const TempTestDirectory&) = delete;
        TempTestDirectory& operator=(const TempTestDirectory&) = delete;

        ~TempTestDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        [[nodiscard]] std::string wal_path() const {
            return path_ + "/execution.wal";
        }

    private:
        std::string path_;
    };

    inline std::vector<std::uint8_t> read_file_bytes(const std::string& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            throw std::runtime_error("failed to open test file: " + path);
        }
        const std::streamsize size = input.tellg();
        if (size < 0) {
            throw std::runtime_error("failed to size test file: " + path);
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        if (!input || (size > 0 && !input.read(
                reinterpret_cast<char*>(bytes.data()), size))) {
            throw std::runtime_error("failed to read test file: " + path);
        }
        return bytes;
    }
}  // namespace exchange::test
