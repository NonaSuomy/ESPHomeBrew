# network for Tulip on the ESP32-P4 PAPP loader.
#
# The device's firmware owns the network (Ethernet or Wi-Fi, from its
# ESPHome configuration) and Tulip uses it as it is. WLAN(STA_IF) reports that
# network: isconnected(), ifconfig() and status() come from the loader
# (net_ipv4). connect() and the other setup calls change nothing; they say
# so. tulip.ip() and tulip.wifi() work through this module unchanged.
import _pappnet

STA_IF = 0
AP_IF = 1
STAT_IDLE = 1000
STAT_CONNECTING = 1001
STAT_GOT_IP = 1010
STAT_NO_AP_FOUND = 201
STAT_WRONG_PASSWORD = 202
STAT_BEACON_TIMEOUT = 200
STAT_ASSOC_FAIL = 203
STAT_HANDSHAKE_TIMEOUT = 204

_NO_ADDRESS = ("0.0.0.0", "0.0.0.0", "0.0.0.0", "0.0.0.0")
_DEVICE_NETWORK = "Tulip uses this device's own network (set up in its firmware); there is nothing to connect here."


def _ipv4():
    # (ip, netmask) while the device's network is up, else None.
    if not _pappnet.has("ipv4"):
        return None
    return _pappnet.ipv4()


def hostname(name=None):
    if name is not None:
        print(_DEVICE_NETWORK)
    return "tulip"


def country(code=None):
    return "XX"


class WLAN:
    def __init__(self, interface=STA_IF):
        self._sta = interface == STA_IF

    def active(self, *args):
        return self._sta

    def connect(self, *args, **kwargs):
        if not _pappnet.has("ipv4"):
            print("This PAPP loader is too old to give Tulip a network (no net_ipv4); update the loader.")
        else:
            print(_DEVICE_NETWORK)

    def disconnect(self):
        pass

    def isconnected(self):
        return self._sta and _ipv4() is not None

    def ifconfig(self, *args):
        if args:
            print(_DEVICE_NETWORK)
            return None
        up = _ipv4() if self._sta else None
        if up is None:
            return _NO_ADDRESS
        return (up[0], up[1], "0.0.0.0", "0.0.0.0")

    def ipconfig(self, *args, **kwargs):
        if kwargs:
            print(_DEVICE_NETWORK)
            return None
        up = _ipv4() if self._sta else None
        values = {"addr4": (up[0], up[1]) if up else ("0.0.0.0", "0.0.0.0"), "dhcp4": True, "gw4": "0.0.0.0"}
        if len(args) == 1:
            if args[0] not in values:
                raise ValueError("unknown config param")
            return values[args[0]]
        return None

    def status(self, param=None):
        if param is None:
            return STAT_GOT_IP if self.isconnected() else STAT_IDLE
        if param == "rssi":
            return 0
        raise ValueError("unknown status param")

    def scan(self):
        return []

    def config(self, *args, **kwargs):
        if kwargs:
            return None
        if len(args) != 1:
            raise TypeError("config() takes one parameter name, or keyword arguments")
        key = args[0]
        if key == "mac":
            return bytes(6)
        if key in ("essid", "ssid"):
            return ""
        if key in ("hostname", "dhcp_hostname"):
            return "tulip"
        if key in ("channel", "txpower", "security", "reconnects", "pm"):
            return 0
        raise ValueError("unknown config param")


# The same view of the device's network for code written for wired boards.
LAN = WLAN
