"""Driver DHO804 USBTMC e ponte SCPI local; importar nao abre hardware."""
from .driver import (
    UsbTmcInstrument, ScpiUsbBridge, descobrir_dho804,
    UsbTmcError, UsbTmcBusyError, UsbTmcPermissionError,
    UsbTmcTimeoutError, UsbTmcDiscoveryError, UsbTmcAmbiguousError,
)

__version__ = "0.3.0"
__all__ = [
    "UsbTmcInstrument", "ScpiUsbBridge", "descobrir_dho804",
    "UsbTmcError", "UsbTmcBusyError", "UsbTmcPermissionError",
    "UsbTmcTimeoutError", "UsbTmcDiscoveryError", "UsbTmcAmbiguousError",
]
