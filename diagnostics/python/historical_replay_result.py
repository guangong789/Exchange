"""Historical replay values using the exact C++ TradingResult names."""

from enum import Enum

from .partial_fill_diagnosis import _integer


class ReplayResult(str, Enum):
    ACCEPTED = "Accepted"
    CANCELLED = "Cancelled"
    ACCOUNT_NOT_FOUND = "AccountNotFound"
    INSUFFICIENT_FUNDS = "InsufficientFunds"
    DUPLICATE_ORDER = "DuplicateOrder"
    INVALID_ORDER = "InvalidOrder"
    COUNTERPARTY_NOT_ACCOUNT_BACKED = "CounterpartyNotAccountBacked"
    CANCEL_NOT_FOUND = "CancelNotFound"
    CANCEL_NOT_OWNER = "CancelNotOwner"
    INVALID_REQUEST = "InvalidRequest"


SUBMIT_RESULTS = frozenset({
    ReplayResult.ACCEPTED, ReplayResult.ACCOUNT_NOT_FOUND,
    ReplayResult.INSUFFICIENT_FUNDS, ReplayResult.DUPLICATE_ORDER,
    ReplayResult.INVALID_ORDER, ReplayResult.COUNTERPARTY_NOT_ACCOUNT_BACKED,
})
CANCEL_RESULTS = frozenset({
    ReplayResult.CANCELLED, ReplayResult.ACCOUNT_NOT_FOUND,
    ReplayResult.CANCEL_NOT_FOUND, ReplayResult.CANCEL_NOT_OWNER,
})
OUTCOME_BASIS = "deterministic_replay"
UINT64_MAX = (1 << 64) - 1
INT64_MAX = (1 << 63) - 1
UINT32_MAX = (1 << 32) - 1


def bounded_integer(value: object, path: str, minimum: int = 1,
                    maximum: int = UINT64_MAX) -> int:
    number = _integer(value, path, minimum)
    if number > maximum:
        raise ValueError(f"{path} exceeds the C++ integer range")
    return number
