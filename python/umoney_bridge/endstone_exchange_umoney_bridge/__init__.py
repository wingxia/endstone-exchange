from .server import (
    BridgeJournal,
    BridgeOperationError,
    BridgeState,
    DirectDispatcher,
    MainThreadDispatcher,
    make_handler,
)

try:
    from .plugin import UMoneyBridgePlugin
except ModuleNotFoundError as error:
    if error.name is None or not error.name.startswith("endstone"):
        raise
    UMoneyBridgePlugin = None  # type: ignore[assignment]

__all__ = [
    "BridgeJournal",
    "BridgeOperationError",
    "BridgeState",
    "DirectDispatcher",
    "MainThreadDispatcher",
    "UMoneyBridgePlugin",
    "make_handler",
]
