#include "exchange/accounting/ledger.hpp"

#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace exchange {
    namespace {
#if defined(__SIZEOF_INT128__)
        __extension__ typedef __int128 WideAmount;
#else
#error "ledger validation requires compiler support for signed 128-bit integers"
#endif

        static_assert(std::is_nothrow_move_constructible_v<LedgerEntry>);
        static_assert(
            std::is_nothrow_move_constructible_v<std::vector<LedgerEntry>>);

        [[nodiscard]] bool is_valid_bucket(BalanceBucket bucket) noexcept {
            return bucket == BalanceBucket::Available
                || bucket == BalanceBucket::Reserved;
        }

        void validate_posting(const Posting& posting) {
            if (posting.account_id == 0) {
                throw std::invalid_argument(
                    "posting account ID must be non-zero");
            }
            if (posting.asset_id == 0) {
                throw std::invalid_argument(
                    "posting asset ID must be non-zero");
            }
            if (!is_valid_bucket(posting.bucket)) {
                throw std::invalid_argument("invalid posting balance bucket");
            }
            if (posting.delta == 0) {
                throw std::invalid_argument(
                    "posting delta must be non-zero");
            }
            if (posting.delta == std::numeric_limits<Amount>::min()) {
                throw std::invalid_argument(
                    "posting delta must be safely negatable");
            }
        }

        void validate_order_id(OrderId order_id) {
            if (order_id == 0) {
                throw std::invalid_argument(
                    "ledger metadata order ID must be non-zero");
            }
        }

        void validate_reservation_builder_input(
            OrderId order_id,
            AccountId account_id,
            AssetId asset_id,
            Amount amount) {
            validate_order_id(order_id);
            if (account_id == 0) {
                throw std::invalid_argument(
                    "Ledger transaction account ID must be non-zero");
            }
            if (asset_id == 0) {
                throw std::invalid_argument(
                    "Ledger transaction asset ID must be non-zero");
            }
            if (amount <= 0) {
                throw std::invalid_argument(
                    "Ledger transaction amount must be positive");
            }
        }

        void validate_trade_metadata(const Trade& trade) {
            validate_order_id(trade.buy_order_id);
            validate_order_id(trade.sell_order_id);
            if (trade.price <= 0) {
                throw std::invalid_argument(
                    "ledger Trade price must be positive");
            }
            if (trade.quantity <= 0) {
                throw std::invalid_argument(
                    "ledger Trade quantity must be positive");
            }
        }

        void validate_reserve_shape(const std::vector<Posting>& postings) {
            if (postings.size() != 2) {
                throw std::invalid_argument(
                    "Reserve transaction must contain exactly two postings");
            }

            const Posting& available = postings[0];
            const Posting& reserved = postings[1];
            if (available.account_id != reserved.account_id
                || available.asset_id != reserved.asset_id
                || available.bucket != BalanceBucket::Available
                || reserved.bucket != BalanceBucket::Reserved
                || available.delta >= 0
                || reserved.delta <= 0
                || available.delta != -reserved.delta) {
                throw std::invalid_argument(
                    "invalid canonical Reserve transaction shape");
            }
        }

        void validate_release_shape(const std::vector<Posting>& postings) {
            if (postings.size() != 2) {
                throw std::invalid_argument(
                    "Release transaction must contain exactly two postings");
            }

            const Posting& reserved = postings[0];
            const Posting& available = postings[1];
            if (reserved.account_id != available.account_id
                || reserved.asset_id != available.asset_id
                || reserved.bucket != BalanceBucket::Reserved
                || available.bucket != BalanceBucket::Available
                || reserved.delta >= 0
                || available.delta <= 0
                || reserved.delta != -available.delta) {
                throw std::invalid_argument(
                    "invalid canonical Release transaction shape");
            }
        }

        void validate_trade_shape(const std::vector<Posting>& postings) {
            if (postings.size() != 4) {
                throw std::invalid_argument(
                    "Trade transaction must contain exactly four postings");
            }

            const Posting& buyer_quote = postings[0];
            const Posting& seller_base = postings[1];
            const Posting& buyer_base = postings[2];
            const Posting& seller_quote = postings[3];

            if (buyer_quote.bucket != BalanceBucket::Reserved
                || seller_base.bucket != BalanceBucket::Reserved
                || buyer_base.bucket != BalanceBucket::Available
                || seller_quote.bucket != BalanceBucket::Available
                || buyer_quote.delta >= 0
                || seller_base.delta >= 0
                || buyer_base.delta <= 0
                || seller_quote.delta <= 0
                || buyer_quote.account_id != buyer_base.account_id
                || seller_base.account_id != seller_quote.account_id
                || buyer_quote.asset_id != seller_quote.asset_id
                || seller_base.asset_id != buyer_base.asset_id
                || buyer_quote.asset_id == seller_base.asset_id
                || buyer_quote.delta != -seller_quote.delta
                || seller_base.delta != -buyer_base.delta) {
                throw std::invalid_argument(
                    "invalid canonical Trade transaction shape");
            }
        }

        void validate_funding_shape(
            const FundingLedgerMetadata& metadata,
            const std::vector<Posting>& postings) {
            if (postings.size() != 2) {
                throw std::invalid_argument(
                    "Funding transaction must contain exactly two postings");
            }

            const Posting& source = postings[0];
            const Posting& destination = postings[1];
            if (metadata.source_account_id == 0
                || metadata.destination_account_id == 0
                || metadata.source_account_id
                    == metadata.destination_account_id
                || source.account_id != metadata.source_account_id
                || destination.account_id
                    != metadata.destination_account_id
                || source.account_id == destination.account_id
                || source.asset_id != destination.asset_id
                || source.bucket != BalanceBucket::Available
                || destination.bucket != BalanceBucket::Available
                || source.delta >= 0
                || destination.delta <= 0
                || source.delta != -destination.delta) {
                throw std::invalid_argument(
                    "invalid canonical Funding transaction shape");
            }
        }

        void validate_asset_balancing(
            const std::vector<Posting>& postings) {
            std::map<AssetId, WideAmount> sums;
            for (const Posting& posting : postings) {
                sums[posting.asset_id] +=
                    static_cast<WideAmount>(posting.delta);
            }

            for (const auto& [asset_id, sum] : sums) {
                static_cast<void>(asset_id);
                if (sum != 0) {
                    throw std::invalid_argument(
                        "Ledger transaction is not balanced per Asset");
                }
            }
        }

        void validate_transaction(const LedgerTransaction& transaction) {
            if (transaction.postings.empty()) {
                throw std::invalid_argument(
                    "Ledger transaction must contain postings");
            }
            for (const Posting& posting : transaction.postings) {
                validate_posting(posting);
            }

            if (transaction.metadata.valueless_by_exception()) {
                throw std::invalid_argument("invalid Ledger metadata");
            }

            std::visit(
                [&](const auto& metadata) {
                    using Metadata = std::decay_t<decltype(metadata)>;
                    if constexpr (std::is_same_v<
                                      Metadata,
                                      ReserveLedgerMetadata>) {
                        validate_order_id(metadata.order_id);
                        validate_reserve_shape(transaction.postings);
                    } else if constexpr (std::is_same_v<
                                             Metadata,
                                             ReleaseLedgerMetadata>) {
                        validate_order_id(metadata.order_id);
                        validate_release_shape(transaction.postings);
                    } else if constexpr (std::is_same_v<
                                             Metadata,
                                             TradeLedgerMetadata>) {
                        validate_trade_metadata(metadata.trade);
                        validate_trade_shape(transaction.postings);
                    } else {
                        validate_funding_shape(
                            metadata,
                            transaction.postings);
                    }
                },
                transaction.metadata);

            validate_asset_balancing(transaction.postings);
        }
    }  // namespace

    LedgerTransaction make_reserve_ledger_transaction(
        OrderId order_id,
        AccountId account_id,
        AssetId asset_id,
        Amount amount) {
        validate_reservation_builder_input(
            order_id,
            account_id,
            asset_id,
            amount);

        LedgerTransaction transaction{
            ReserveLedgerMetadata{order_id},
            {
                Posting{
                    account_id,
                    asset_id,
                    BalanceBucket::Available,
                    -amount},
                Posting{
                    account_id,
                    asset_id,
                    BalanceBucket::Reserved,
                    amount},
            },
        };
        validate_transaction(transaction);
        return transaction;
    }

    LedgerTransaction make_release_ledger_transaction(
        OrderId order_id,
        AccountId account_id,
        AssetId asset_id,
        Amount amount) {
        validate_reservation_builder_input(
            order_id,
            account_id,
            asset_id,
            amount);

        LedgerTransaction transaction{
            ReleaseLedgerMetadata{order_id},
            {
                Posting{
                    account_id,
                    asset_id,
                    BalanceBucket::Reserved,
                    -amount},
                Posting{
                    account_id,
                    asset_id,
                    BalanceBucket::Available,
                    amount},
            },
        };
        validate_transaction(transaction);
        return transaction;
    }

    LedgerTransaction make_trade_ledger_transaction(
        const InstrumentContext& instrument,
        const Trade& trade,
        AccountId buyer_account_id,
        AccountId seller_account_id) {
        if (buyer_account_id == 0) {
            throw std::invalid_argument(
                "buyer account ID must be non-zero");
        }
        if (seller_account_id == 0) {
            throw std::invalid_argument(
                "seller account ID must be non-zero");
        }

        const TradeFinancialAmounts amounts = calculate_trade_amounts(
            instrument,
            trade.price,
            trade.quantity);
        LedgerTransaction transaction{
            TradeLedgerMetadata{trade},
            {
                Posting{
                    buyer_account_id,
                    instrument.quote_asset,
                    BalanceBucket::Reserved,
                    -amounts.quote_amount},
                Posting{
                    seller_account_id,
                    instrument.base_asset,
                    BalanceBucket::Reserved,
                    -amounts.base_amount},
                Posting{
                    buyer_account_id,
                    instrument.base_asset,
                    BalanceBucket::Available,
                    amounts.base_amount},
                Posting{
                    seller_account_id,
                    instrument.quote_asset,
                    BalanceBucket::Available,
                    amounts.quote_amount},
            },
        };
        validate_transaction(transaction);
        return transaction;
    }

    LedgerTransaction make_funding_ledger_transaction(
        AccountId source_account_id,
        AccountId destination_account_id,
        AssetId asset_id,
        Amount amount) {
        if (source_account_id == 0) {
            throw std::invalid_argument(
                "Funding source account ID must be non-zero");
        }
        if (destination_account_id == 0) {
            throw std::invalid_argument(
                "Funding destination account ID must be non-zero");
        }
        if (source_account_id == destination_account_id) {
            throw std::invalid_argument(
                "Funding accounts must be distinct");
        }
        if (asset_id == 0) {
            throw std::invalid_argument(
                "Funding asset ID must be non-zero");
        }
        if (amount <= 0) {
            throw std::invalid_argument(
                "Funding amount must be positive");
        }

        LedgerTransaction transaction{
            FundingLedgerMetadata{
                source_account_id,
                destination_account_id},
            {
                Posting{
                    source_account_id,
                    asset_id,
                    BalanceBucket::Available,
                    -amount},
                Posting{
                    destination_account_id,
                    asset_id,
                    BalanceBucket::Available,
                    amount},
            },
        };
        validate_transaction(transaction);
        return transaction;
    }

    Ledger::PreparedBatch::PreparedBatch(
        Ledger& ledger,
        std::vector<LedgerEntry>&& staged_entries,
        LedgerSequence next_sequence_after_commit,
        bool sequence_exhausted_after_commit) noexcept
        : ledger_(&ledger),
          staged_entries_(std::move(staged_entries)),
          next_sequence_after_commit_(next_sequence_after_commit),
          sequence_exhausted_after_commit_(
              sequence_exhausted_after_commit) {}

    Ledger::PreparedBatch::PreparedBatch(PreparedBatch&& other) noexcept
        : ledger_(std::exchange(other.ledger_, nullptr)),
          staged_entries_(std::move(other.staged_entries_)),
          next_sequence_after_commit_(
              other.next_sequence_after_commit_),
          sequence_exhausted_after_commit_(
              other.sequence_exhausted_after_commit_) {}

    Ledger::PreparedBatch::~PreparedBatch() noexcept {
        abandon();
    }

    void Ledger::PreparedBatch::commit() noexcept {
        if (ledger_ == nullptr) {
            return;
        }

        Ledger& ledger = *ledger_;
        for (LedgerEntry& entry : staged_entries_) {
            ledger.entries_.push_back(std::move(entry));
        }
        ledger.next_sequence_ = next_sequence_after_commit_;
        ledger.sequence_exhausted_ = sequence_exhausted_after_commit_;
        ledger.has_prepared_batch_ = false;
        ledger_ = nullptr;
    }

    void Ledger::PreparedBatch::abandon() noexcept {
        if (ledger_ == nullptr) {
            return;
        }

        ledger_->has_prepared_batch_ = false;
        ledger_ = nullptr;
    }

    Ledger::PreparedBatch Ledger::prepare_batch(
        std::vector<LedgerTransaction> transactions) {
        if (transactions.empty()) {
            return PreparedBatch{};
        }
        if (has_prepared_batch_) {
            throw std::logic_error(
                "Ledger already has an active prepared batch");
        }

        for (const LedgerTransaction& transaction : transactions) {
            validate_transaction(transaction);
        }

        if (sequence_exhausted_) {
            throw std::overflow_error("Ledger sequence is exhausted");
        }

        constexpr LedgerSequence maximum =
            std::numeric_limits<LedgerSequence>::max();
        if (transactions.size() - 1
            > maximum - next_sequence_) {
            throw std::overflow_error("Ledger sequence overflow");
        }

        std::vector<LedgerEntry> staged;
        staged.reserve(transactions.size());

        LedgerSequence sequence = next_sequence_;
        for (LedgerTransaction& transaction : transactions) {
            staged.push_back(LedgerEntry{
                sequence,
                std::move(transaction),
            });
            if (sequence != maximum) {
                ++sequence;
            }
        }

        if (staged.size() > entries_.max_size() - entries_.size()) {
            throw std::length_error("Ledger entry capacity overflow");
        }
        entries_.reserve(entries_.size() + staged.size());

        const bool sequence_exhausted_after_commit =
            staged.back().sequence == maximum;
        const LedgerSequence next_sequence_after_commit =
            sequence_exhausted_after_commit ? maximum : sequence;

        has_prepared_batch_ = true;
        return PreparedBatch{
            *this,
            std::move(staged),
            next_sequence_after_commit,
            sequence_exhausted_after_commit};
    }

    void Ledger::append(LedgerTransaction transaction) {
        std::vector<LedgerTransaction> transactions;
        transactions.reserve(1);
        transactions.push_back(std::move(transaction));
        append_batch(std::move(transactions));
    }

    void Ledger::append_batch(
        std::vector<LedgerTransaction> transactions) {
        auto prepared = prepare_batch(std::move(transactions));
        prepared.commit();
    }

    const std::vector<LedgerEntry>& Ledger::entries() const noexcept {
        return entries_;
    }
}  // namespace exchange
