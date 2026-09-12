#include "execution/trading_bootstrap.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace exchange {
    namespace {
        constexpr std::array<std::uint32_t, 64> sha256_constants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

        template <typename Integer>
        void append_little_endian(
            std::vector<std::uint8_t>& bytes,
            Integer value) {
            using Unsigned = std::make_unsigned_t<Integer>;
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

        TradingBootstrapConfig canonical_bootstrap(
            const TradingBootstrapConfig& bootstrap) {
            if (bootstrap.accounts.size()
                > std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("too many bootstrap accounts");
            }

            TradingBootstrapConfig canonical = bootstrap;
            std::sort(
                canonical.accounts.begin(),
                canonical.accounts.end(),
                [](const BootstrapAccount& left,
                   const BootstrapAccount& right) {
                    return left.account_id < right.account_id;
                });

            AccountId previous_account{};
            for (BootstrapAccount& account : canonical.accounts) {
                if (account.account_id == 0
                    || account.account_id == previous_account) {
                    throw std::invalid_argument(
                        "bootstrap account IDs must be unique and non-zero");
                }
                previous_account = account.account_id;
                if (account.balances.size()
                    > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::length_error(
                        "too many bootstrap balance rows");
                }
                std::sort(
                    account.balances.begin(),
                    account.balances.end(),
                    [](const BootstrapBalance& left,
                       const BootstrapBalance& right) {
                        return left.asset_id < right.asset_id;
                    });

                AssetId previous_asset{};
                for (const BootstrapBalance& balance : account.balances) {
                    if (balance.asset_id == 0
                        || balance.asset_id == previous_asset) {
                        throw std::invalid_argument(
                            "bootstrap asset IDs must be unique and non-zero");
                    }
                    previous_asset = balance.asset_id;
                    if (balance.balance.available < 0
                        || balance.balance.reserved < 0
                        || (balance.balance.available == 0
                            && balance.balance.reserved == 0)
                        || balance.balance.reserved
                            > std::numeric_limits<Amount>::max()
                                  - balance.balance.available) {
                        throw std::invalid_argument(
                            "invalid bootstrap balance row");
                    }
                }
            }
            return canonical;
        }

        BootstrapFingerprint sha256(std::vector<std::uint8_t> bytes) {
            if (bytes.size()
                > std::numeric_limits<std::uint64_t>::max() / 8U) {
                throw std::length_error("bootstrap encoding is too large");
            }
            const std::uint64_t bit_length =
                static_cast<std::uint64_t>(bytes.size()) * 8U;
            bytes.push_back(0x80U);
            while (bytes.size() % 64 != 56) {
                bytes.push_back(0);
            }
            for (int shift = 56; shift >= 0; shift -= 8) {
                bytes.push_back(static_cast<std::uint8_t>(
                    bit_length >> shift));
            }

            std::array<std::uint32_t, 8> state{
                0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
            for (std::size_t block = 0; block < bytes.size(); block += 64) {
                std::array<std::uint32_t, 64> words{};
                for (std::size_t index = 0; index < 16; ++index) {
                    const std::size_t offset = block + index * 4;
                    words[index] =
                        (static_cast<std::uint32_t>(bytes[offset]) << 24U)
                        | (static_cast<std::uint32_t>(bytes[offset + 1])
                           << 16U)
                        | (static_cast<std::uint32_t>(bytes[offset + 2])
                           << 8U)
                        | static_cast<std::uint32_t>(bytes[offset + 3]);
                }
                for (std::size_t index = 16; index < 64; ++index) {
                    const std::uint32_t s0 =
                        std::rotr(words[index - 15], 7)
                        ^ std::rotr(words[index - 15], 18)
                        ^ (words[index - 15] >> 3U);
                    const std::uint32_t s1 =
                        std::rotr(words[index - 2], 17)
                        ^ std::rotr(words[index - 2], 19)
                        ^ (words[index - 2] >> 10U);
                    words[index] = words[index - 16] + s0
                        + words[index - 7] + s1;
                }

                std::uint32_t a = state[0];
                std::uint32_t b = state[1];
                std::uint32_t c = state[2];
                std::uint32_t d = state[3];
                std::uint32_t e = state[4];
                std::uint32_t f = state[5];
                std::uint32_t g = state[6];
                std::uint32_t h = state[7];
                for (std::size_t index = 0; index < 64; ++index) {
                    const std::uint32_t sum1 = std::rotr(e, 6)
                        ^ std::rotr(e, 11) ^ std::rotr(e, 25);
                    const std::uint32_t choice = (e & f) ^ (~e & g);
                    const std::uint32_t temporary1 = h + sum1 + choice
                        + sha256_constants[index] + words[index];
                    const std::uint32_t sum0 = std::rotr(a, 2)
                        ^ std::rotr(a, 13) ^ std::rotr(a, 22);
                    const std::uint32_t majority =
                        (a & b) ^ (a & c) ^ (b & c);
                    const std::uint32_t temporary2 = sum0 + majority;
                    h = g;
                    g = f;
                    f = e;
                    e = d + temporary1;
                    d = c;
                    c = b;
                    b = a;
                    a = temporary1 + temporary2;
                }
                state[0] += a;
                state[1] += b;
                state[2] += c;
                state[3] += d;
                state[4] += e;
                state[5] += f;
                state[6] += g;
                state[7] += h;
            }

            BootstrapFingerprint digest{};
            for (std::size_t index = 0; index < state.size(); ++index) {
                digest[index * 4] = static_cast<std::uint8_t>(
                    state[index] >> 24U);
                digest[index * 4 + 1] = static_cast<std::uint8_t>(
                    state[index] >> 16U);
                digest[index * 4 + 2] = static_cast<std::uint8_t>(
                    state[index] >> 8U);
                digest[index * 4 + 3] =
                    static_cast<std::uint8_t>(state[index]);
            }
            return digest;
        }
    }  // namespace

    BootstrapFingerprint calculate_bootstrap_fingerprint(
        const TradingBootstrapConfig& bootstrap) {
        const TradingBootstrapConfig canonical =
            canonical_bootstrap(bootstrap);
        std::vector<std::uint8_t> bytes{
            'E', 'X', 'B', 'O', 'O', 'T', '0', '1'};
        append_little_endian(
            bytes,
            static_cast<std::uint32_t>(canonical.accounts.size()));
        for (const BootstrapAccount& account : canonical.accounts) {
            append_little_endian(bytes, account.account_id);
            append_little_endian(
                bytes,
                static_cast<std::uint32_t>(account.balances.size()));
            for (const BootstrapBalance& balance : account.balances) {
                append_little_endian(bytes, balance.asset_id);
                append_little_endian(bytes, balance.balance.available);
                append_little_endian(bytes, balance.balance.reserved);
            }
        }
        return sha256(std::move(bytes));
    }

    void apply_trading_bootstrap(
        const TradingBootstrapConfig& bootstrap,
        AccountStore& accounts) {
        const TradingBootstrapConfig canonical =
            canonical_bootstrap(bootstrap);
        for (const BootstrapAccount& account : canonical.accounts) {
            if (!accounts.create_account(account.account_id)) {
                throw std::logic_error("duplicate bootstrap account");
            }
            for (const BootstrapBalance& balance : account.balances) {
                const Amount total = balance.balance.available
                    + balance.balance.reserved;
                accounts.fund(account.account_id, balance.asset_id, total);
                if (balance.balance.reserved > 0
                    && accounts.reserve(
                           account.account_id,
                           balance.asset_id,
                           balance.balance.reserved)
                           != ReserveResult::Success) {
                    throw std::logic_error(
                        "bootstrap reserve unexpectedly failed");
                }
            }
        }
    }
}  // namespace exchange
