#!/usr/bin/env python3
"""Map crash addresses from a device log to functions in a PAPP.

A PAPP is linked at 0x4A000000, so PC/backtrace addresses 0x4A...... in an
ESP-IDF crash report are inside the app. The dev builds publish a symbol list
(`nm -n -C`) next to each work-in-progress .papp.

    python3 tools/papp_symbolize.py redalert.sym 0x4A01234C 0x4A00ABCD
    python3 tools/papp_symbolize.py --release redalert 0x4A01234C   # fetch the dev-builds list

Addresses outside the app (0x40..., 0x4FF...) are loader/ESP-IDF code: look
those up in the firmware ELF with addr2line.
"""

from __future__ import annotations

import argparse
import bisect
import re
import sys
import urllib.request
from pathlib import Path

DEV_BUILDS = "https://github.com/NonaSuomy/esphomebrew/releases/download/dev-builds/{app}.sym"
LINE = re.compile(r"^([0-9a-fA-F]+)\s+([A-Za-z])\s+(.+)$")


def load_symbols(text: str) -> list[tuple[int, str, bool]]:
    """(address, name, is_code) for every symbol: stacks also hold data pointers."""
    symbols = []
    for line in text.splitlines():
        m = LINE.match(line.strip())
        if m:
            symbols.append((int(m.group(1), 16), m.group(3), m.group(2) in "TtWw"))
    symbols.sort()
    return symbols


def symbolize(symbols: list[tuple[int, str, bool]], address: int) -> str:
    if not 0x4A000000 <= address < 0x4C000000:
        return "outside the app (loader / ESP-IDF: use addr2line on the firmware ELF)"
    i = bisect.bisect_right([s[0] for s in symbols], address) - 1
    if i < 0:
        return "before the first symbol"
    base, name, code = symbols[i]
    if address - base >= 0x4000:
        return "no symbol (unnamed data such as strings)"
    return f"{'' if code else 'data: '}{name} + 0x{address - base:x}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("symbols", help="a .sym file, or with --release the app name")
    parser.add_argument("addresses", nargs="+")
    parser.add_argument("--release", action="store_true", help="download <app>.sym from the dev-builds prerelease")
    args = parser.parse_args()
    if args.release:
        with urllib.request.urlopen(DEV_BUILDS.format(app=args.symbols), timeout=60) as response:
            text = response.read().decode()
    else:
        text = Path(args.symbols).read_text()
    symbols = load_symbols(text)
    for raw in args.addresses:
        address = int(raw, 16)
        print(f"0x{address:08x}  {symbolize(symbols, address)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
