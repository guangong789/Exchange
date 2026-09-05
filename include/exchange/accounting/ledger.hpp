#pragma once

#include "exchange/core/types.hpp"

#include <cstdint>
#include <variant>
#include <vector>

#include "exchange/accounting/account.hpp"
#include "exchange/accounting/financial_conversion.hpp"
#include "exchange/matching/trade.hpp"

namespace exchange {
    using LedgerSequence = std::uint64_t;

    enum class BalanceBucket : std::uint8_t {
        Available,
        Reserved,
    };

    struct Posting {
        AccountId account_id{};
        AssetId asset_id{};
        BalanceBucket bucket{};
        Amount delta{};

        bool operator==(const Posting&) const = default;
    };

    struct ReserveLedgerMetadata {
        OrderId order_id{};

        bool operator==(const ReserveLedgerMetadata&) const = default;
    };

    struct ReleaseLedgerMetadata {
        OrderId order_id{};

        bool operator==(const ReleaseLedgerMetadata&) const = default;
    };

    struct TradeLedgerMetadata {
        Trade trade;

        bool operator==(const TradeLedgerMetadata&) const = default;
    };

    struct FundingLedgerMetadata {
        AccountId source_account_id{};
        AccountId destination_account_id{};

        bool operator==(const FundingLedgerMetadata&) const = default;
    };

    using LedgerMetadata = std::variant<
        ReserveLedgerMetadata,
        ReleaseLedgerMetadata,
        TradeLedgerMetadata,
        FundingLedgerMetadata>;

    struct LedgerTransaction {
        LedgerMetadata metadata;
        std::vector<Posting> postings;

        bool operator==(const LedgerTransaction&) const = default;
    };

    struct LedgerEntry {
        LedgerSequence sequence{};
        LedgerTransaction transaction;

        bool operator==(const LedgerEntry&) const = default;
    };

    [[nodiscard]] LedgerTransaction make_reserve_ledger_transaction(
        OrderId order_id,
        AccountId account_id,
        AssetId asset_id,
        Amount amount);

    [[nodiscard]] LedgerTransaction make_release_ledger_transaction(
        OrderId order_id,
        AccountId account_id,
        AssetId asset_id,
        Amount amount);

    [[nodiscard]] LedgerTransaction make_trade_ledger_transaction(
        const InstrumentContext& instrument,
        const Trade& trade,
        AccountId buyer_account_id,
        AccountId seller_account_id);

    [[nodiscard]] LedgerTransaction make_funding_ledger_transaction(
        AccountId source_account_id,
        AccountId destination_account_id,
        AssetId asset_id,
        Amount amount);

    class Ledger {
    public:
        class PreparedBatch {
        public:
            // The originating Ledger must outlive an active handle.
            PreparedBatch(const PreparedBatch&) = delete;
            PreparedBatch& operator=(const PreparedBatch&) = delete;
            PreparedBatch(PreparedBatch&& other) noexcept;
            PreparedBatch& operator=(PreparedBatch&&) = delete;
            ~PreparedBatch() noexcept;

            void commit() noexcept;

        private:
            friend class Ledger;

            PreparedBatch() noexcept = default;
            PreparedBatch(
                Ledger& ledger,
                std::vector<LedgerEntry>&& staged_entries,
                LedgerSequence next_sequence_after_commit,
                bool sequence_exhausted_after_commit) noexcept;

            void abandon() noexcept;

            Ledger* ledger_{};
            std::vector<LedgerEntry> staged_entries_;
            LedgerSequence next_sequence_after_commit_{};
            bool sequence_exhausted_after_commit_{};
        };

        Ledger() = default;
        Ledger(const Ledger&) = delete;
        Ledger& operator=(const Ledger&) = delete;
        Ledger(Ledger&&) = delete;
        Ledger& operator=(Ledger&&) = delete;

        [[nodiscard]] PreparedBatch prepare_batch(
            std::vector<LedgerTransaction> transactions);

        void append(LedgerTransaction transaction);

        void append_batch(
            std::vector<LedgerTransaction> transactions);

        [[nodiscard]] const std::vector<LedgerEntry>& entries() const noexcept;

    private:
        std::vector<LedgerEntry> entries_;
        LedgerSequence next_sequence_{1};
        bool sequence_exhausted_{};
        bool has_prepared_batch_{};
    };
}  // namespace exchange
