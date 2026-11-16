#!/usr/bin/env python3
"""Drive the dev Switch from the Mac: screenshots, buttons, FTP, nxlink.

Needs on the Switch: sys-botbase (port 6000) and sys-ftpd (port 5000).
Needs on the Mac:
  ~/.config/warppoint/switch-ip     the Switch's IP address (kept out of the repo)
  Keychain item "warppoint-switch-ftp" (account "warp") holding the FTP password

Usage:
  switch.py shot [out.jpg]          screenshot (default build/switch/shot.jpg)
  switch.py click A|B|X|Y|L|R|ZL|ZR|PLUS|MINUS|HOME|DUP|DDOWN|DLEFT|DRIGHT|...
  switch.py press BUTTON / release BUTTON   hold / let go
  switch.py detach                  remove sys-botbase's virtual controller
  switch.py hbmenu                  from the Home menu: open hbmenu with full memory
                                    (hold R while launching the first game)
  switch.py netloader               press Y in hbmenu to wait for nxlink
  switch.py run app.nro [secs]      send app over nxlink, stream its output
  switch.py ls PATH | get REMOTE [LOCAL] | put LOCAL REMOTE    FTP
"""
import ftplib
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BOTBASE_PORT = 6000
FTP_PORT = 5000
NXLINK = "/opt/devkitpro/tools/bin/nxlink"


def switch_ip() -> str:
    if os.environ.get("WARP_SWITCH_IP"):
        return os.environ["WARP_SWITCH_IP"]
    cfg = Path.home() / ".config/warppoint/switch-ip"
    if not cfg.exists():
        sys.exit(f"Put the Switch's IP in {cfg} (or set WARP_SWITCH_IP).")
    return cfg.read_text().strip()


# ---- sys-botbase: text commands over TCP, one per line ----

def botbase(command: str, want_reply: bool = False, timeout: float = 10) -> bytes:
    with socket.create_connection((switch_ip(), BOTBASE_PORT), timeout=timeout) as s:
        s.sendall((command + "\r\n").encode())
        if not want_reply:
            return b""
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(1 << 16)
            if not chunk:
                break
            buf += chunk
        return buf.strip()


def shot(out: Path) -> Path:
    # pixelPeek returns the current frame as a hex-encoded JPEG.
    data = bytes.fromhex(botbase("pixelPeek", want_reply=True, timeout=20).decode())
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    return out


def click(button: str, wait: float = 0.4) -> None:
    botbase(f"click {button.upper()}")
    time.sleep(wait)


def open_hbmenu() -> None:
    """Title override: launching a game while holding R opens hbmenu instead,
    with the game's full memory budget. Assumes the Home menu is showing and
    no game is suspended."""
    click("HOME", 1.0)
    click("DUP")                 # make sure the cursor is on the game row
    for _ in range(8):           # go to the leftmost game
        click("DLEFT", 0.15)
    botbase("press R")
    time.sleep(0.3)
    click("A", 3.0)              # launch while R is held
    botbase("release R")
    time.sleep(1.0)


# ---- FTP (sys-ftpd) ----

def ftp() -> ftplib.FTP:
    password = subprocess.run(
        ["security", "find-generic-password", "-s", "warppoint-switch-ftp", "-a", "warp", "-w"],
        capture_output=True, text=True, check=True).stdout.strip()
    f = ftplib.FTP()
    f.connect(switch_ip(), FTP_PORT, timeout=15)
    f.login("warp", password)
    return f


# ---- nxlink ----

def run_nro(nro: str, seconds: float | None) -> int:
    """Send an .nro with nxlink and stream the app's stderr back.

    nxlink buffers its output when stdout isn't a terminal, and that buffer is
    lost if we stop it early. So we hand it a pseudo-terminal (pty) instead of
    a pipe, which makes it print each line as it arrives."""
    import pty
    import select

    # -s keeps nxlink running as a server so the app's stderr streams back here.
    cmd = [NXLINK, "-a", switch_ip(), "-s", nro]
    master, slave = pty.openpty()
    proc = subprocess.Popen(cmd, stdout=slave, stderr=slave, stdin=subprocess.DEVNULL)
    os.close(slave)
    deadline = None if seconds is None else time.monotonic() + seconds
    try:
        while True:
            if deadline is not None and time.monotonic() > deadline:
                print(f"\n[switch.py] stopped listening after {seconds}s (the app may still be running)")
                break
            ready, _, _ = select.select([master], [], [], 0.2)
            if ready:
                try:
                    data = os.read(master, 4096)
                except OSError:  # nxlink exited and closed the pty
                    break
                if not data:
                    break
                sys.stdout.write(data.decode(errors="replace").replace("\r\n", "\n"))
                sys.stdout.flush()
            elif proc.poll() is not None:
                break
    finally:
        if proc.poll() is None:
            proc.terminate()
        proc.wait()
        os.close(master)
    return proc.returncode or 0


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmd, args = argv[0], argv[1:]
    if cmd == "shot":
        print(shot(Path(args[0]) if args else REPO / "build/switch/shot.jpg"))
    elif cmd in ("click", "press", "release"):
        botbase(f"{cmd} {args[0].upper()}")
    elif cmd == "detach":
        botbase("detachController")
    elif cmd == "hbmenu":
        open_hbmenu()
    elif cmd == "netloader":
        click("Y", 1.0)
    elif cmd == "run":
        return run_nro(args[0], float(args[1]) if len(args) > 1 else None)
    elif cmd == "ls":
        for name in ftp().nlst(args[0] if args else "/"):
            print(name)
    elif cmd == "get":
        local = Path(args[1] if len(args) > 1 else Path(args[0]).name)
        with ftp() as f, open(local, "wb") as out:
            f.retrbinary(f"RETR {args[0]}", out.write)
        print(local)
    elif cmd == "put":
        with ftp() as f, open(args[0], "rb") as src:
            f.storbinary(f"STOR {args[1]}", src)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
