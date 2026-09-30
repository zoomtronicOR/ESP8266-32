"""Build data/oui.bin: a small OUI -> vendor table for the device (docs: README "Vendor names").

The full IEEE registry (~37k entries) does not fit on the device, and most of it never
shows up as a WiFi access point. This keeps only vendors of networking and home
equipment that typically appear in a WiFi scan, with short display names.

Usage:
  python tools/build_oui.py                 # download the IEEE registry (MA-L) and build
  python tools/build_oui.py --csv oui.csv   # use a downloaded copy

Format (little endian), read by src/oui.cpp:
  "OUI1" | u16 vendor count | u16 name size (16) | u32 entry count
  vendor names, fixed 16 bytes each (NUL padded)
  entries sorted by OUI: 3 bytes OUI (big endian) + 1 byte vendor index
"""
import csv
import io
import re
import struct
import sys
import urllib.request
from pathlib import Path

IEEE_URL = "https://standards-oui.ieee.org/oui/oui.csv"
OUT = Path(__file__).resolve().parent.parent / "data" / "oui.bin"
NAME_SIZE = 16

# (display name, pattern on the IEEE organization name). First match wins, so more
# specific patterns come first (e.g. "Cisco-Linksys" is Linksys, not Cisco).
VENDORS = [
    ("Linksys", r"linksys"),
    ("Meraki", r"meraki"),
    ("TP-Link", r"\btp-?link\b"),
    ("Mercusys", r"mercusys"),
    ("Netgear", r"\bnetgear\b"),
    ("ASUS", r"\basustek\b|\basus\b"),
    ("Huawei", r"\bhuawei\b"),
    ("Honor", r"honor device"),
    ("ZTE", r"\bzte\b"),
    ("Xiaomi", r"xiaomi"),
    ("Ubiquiti", r"ubiquiti"),
    ("MikroTik", r"mikrotik|routerboard"),
    ("Aruba", r"\baruba\b"),
    ("Ruckus", r"ruckus"),
    ("Juniper", r"juniper networks|mist systems"),
    ("Cisco", r"^cisco"),
    ("D-Link", r"d-link"),
    ("Zyxel", r"zyxel"),
    ("Tenda", r"\btenda\b"),
    ("TOTOLINK", r"zioncom|totolink"),
    ("Cudy", r"\bcudy\b"),
    ("Edimax", r"edimax"),
    ("TRENDnet", r"trendnet"),
    ("Belkin", r"belkin"),
    ("Buffalo", r"\bbuffalo\b"),
    ("AVM", r"\bavm\b"),
    ("Vantiva", r"vantiva"),  # Technicolor's broadband business since 2022
    ("Technicolor", r"technicolor|thomson telecom"),
    ("Sagemcom", r"sagemcom"),
    ("CommScope", r"\barris\b|commscope"),
    ("Nokia", r"\bnokia\b|alcatel-lucent"),
    ("Sercomm", r"sercomm"),
    ("Actiontec", r"actiontec"),
    ("Askey", r"askey"),
    ("FiberHome", r"fiberhome"),
    ("Skyworth", r"skyworth"),
    ("Vodafone", r"vodafone"),
    ("Fortinet", r"fortinet"),
    ("Extreme", r"extreme networks|aerohive"),
    ("Cambium", r"cambium"),
    ("Grandstream", r"grandstream"),
    ("DrayTek", r"draytek"),
    ("Keenetic", r"keenetic"),
    ("Teltonika", r"teltonika"),
    ("Google", r"^google"),
    ("Amazon", r"amazon technologies"),
    ("Apple", r"^apple, inc"),
    ("Samsung", r"samsung electro"),
    ("LG", r"\blg electronics|\blg innotek"),
    ("Sony", r"^sony"),
    ("Microsoft", r"^microsoft"),
    ("Nintendo", r"nintendo"),
    ("OPPO", r"guangdong oppo"),
    ("vivo", r"vivo mobile"),
    ("OnePlus", r"oneplus"),
    ("Espressif", r"espressif"),
    ("Realtek", r"realtek"),
    ("Broadcom", r"broadcom"),
    ("Qualcomm", r"qualcomm|atheros"),
    ("MediaTek", r"mediatek|ralink"),
    ("Intel", r"^intel corporat"),
    ("Foxconn", r"hon hai|foxconn"),
    ("HP", r"hewlett packard|^hp inc"),
    ("Canon", r"^canon inc"),
    ("Epson", r"seiko epson"),
    ("Brother", r"brother industries"),
    ("Roku", r"^roku"),
    ("Sonos", r"^sonos"),
    ("Hikvision", r"hikvision"),
    ("Dahua", r"dahua"),
    ("Tuya", r"\btuya\b"),
]


def load_rows(csv_path):
    if csv_path:
        text = Path(csv_path).read_text(encoding="utf-8", errors="replace")
    else:
        req = urllib.request.Request(IEEE_URL, headers={
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/130.0 Safari/537.36",
            "Accept": "text/csv,*/*", "Accept-Language": "en-US,en;q=0.9"})
        text = urllib.request.urlopen(req, timeout=120).read().decode("utf-8", errors="replace")
    return list(csv.reader(io.StringIO(text)))


def main():
    csv_path = sys.argv[sys.argv.index("--csv") + 1] if "--csv" in sys.argv else None
    rows = load_rows(csv_path)
    compiled = [(name, re.compile(pat, re.I)) for name, pat in VENDORS]
    names = [name for name, _ in VENDORS]
    assert len(names) < 256 and all(len(n.encode()) < NAME_SIZE for n in names)

    entries = {}
    per_vendor = {}
    for row in rows[1:]:
        if len(row) < 3 or row[0] != "MA-L":
            continue
        oui = int(row[1], 16)
        org = row[2].strip()
        for idx, (name, rx) in enumerate(compiled):
            if rx.search(org):
                entries[oui] = idx
                per_vendor[name] = per_vendor.get(name, 0) + 1
                break

    out = bytearray(b"OUI1" + struct.pack("<HHI", len(names), NAME_SIZE, len(entries)))
    for n in names:
        out += n.encode().ljust(NAME_SIZE, b"\0")
    for oui in sorted(entries):
        out += oui.to_bytes(3, "big") + bytes([entries[oui]])
    OUT.write_bytes(out)

    print(f"{len(entries)} OUIs, {len(names)} vendors, {len(out)} bytes -> {OUT}")
    for name in names:
        print(f"  {name:12} {per_vendor.get(name, 0)}")


if __name__ == "__main__":
    main()
