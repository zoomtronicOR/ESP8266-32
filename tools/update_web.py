"""Update the web UI files on the device without losing config.json and history.

`pio run -t uploadfs` writes a fresh LittleFS image built from data/ only, which
erases the saved WiFi network and the history. This script never does that.

Usage:
  python tools/update_web.py --http 192.168.88.65   # over WiFi (fast, device keeps running)
  python tools/update_web.py [COM8]                 # over USB serial, ESP8266 only (works without WiFi)

HTTP mode uploads each file to /api/update/webfile (the same endpoint as the
Update page). Serial mode reads the LittleFS partition, replaces the web files,
and writes it back. The layout matches platformio.ini (eagle.flash.4m2m.ld):
FS at 0x200000, 0x1FA000 bytes, block 8192, page 256.
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FS_OFFSET, FS_SIZE, BLOCK, PAGE = 0x200000, 0x1FA000, 8192, 256
ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"
# The platform pins mklittlefs ~1.203 (LittleFS format must match the firmware)
MKLFS = Path.home() / ".platformio/packages/tool-mklittlefs@1.203.200522/mklittlefs"
if not MKLFS.with_suffix(".exe").exists() and not MKLFS.exists():
    MKLFS = Path.home() / ".platformio/packages/tool-mklittlefs/mklittlefs"


def run(*cmd, cwd=None):
    print(">", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, cwd=cwd)


def web_files():
    """(device path, local file) for everything in data/, pages last."""
    files = [("/" + p.relative_to(DATA).as_posix(), p) for p in DATA.rglob("*") if p.is_file()]
    return sorted(files, key=lambda x: x[0].endswith(".html"))


def update_http(host):
    import requests

    for path, local in web_files():
        with open(local, "rb") as f:
            r = requests.post(
                f"http://{host}/api/update/webfile",
                params={"path": path},
                headers={"X-Requested-With": "wifi-monitor"},
                files={"file": (local.name, f)},
                timeout=60,
            )
        if r.status_code != 200:
            sys.exit(f"{path}: {r.status_code} {r.text}")
        print(f"{path}: {r.json()['bytes']} bytes")
    print("Web files updated (device keeps running). Reload the page in the browser.")


def update_serial(port):
    def esptool(*args):
        run(sys.executable, "-m", "esptool", "--port", port, "--baud", "460800", *args)

    work = Path(tempfile.mkdtemp(prefix="wifimon-fs-"))
    try:
        old_img, new_img, tree = work / "old.bin", work / "new.bin", work / "fs"
        esptool("read_flash", hex(FS_OFFSET), hex(FS_SIZE), old_img)
        # mklittlefs only accepts relative paths, so run it inside the work dir
        run(MKLFS, "-b", BLOCK, "-p", PAGE, "-s", FS_SIZE, "-u", "fs", "old.bin", cwd=work)

        kept = [p.relative_to(tree) for p in tree.rglob("*") if p.is_file()]
        for item in DATA.iterdir():
            target = tree / item.name
            if target.is_dir():
                shutil.rmtree(target)
            elif target.exists():
                target.unlink()
            if item.is_dir():
                shutil.copytree(item, target)
            else:
                shutil.copy2(item, target)
        preserved = [str(p) for p in kept if not (DATA / p.parts[0]).exists()]
        print("Preserved on device:", ", ".join(preserved) or "(nothing)")

        run(MKLFS, "-b", BLOCK, "-p", PAGE, "-s", FS_SIZE, "-c", "fs", "new.bin", cwd=work)
        esptool("write_flash", hex(FS_OFFSET), new_img)
        print("Web files updated; device restarted.")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--http":
        update_http(sys.argv[2])
    else:
        update_serial(sys.argv[1] if len(sys.argv) > 1 else "COM8")
