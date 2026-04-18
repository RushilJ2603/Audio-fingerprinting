"""
shazam_monitor.py  —  STM32 Shazam demo UI over SWV/ITM.

No extra hardware. Uses the ST-Link cable you already have.

Flow:
  1. This script launches `pyocd gdbserver` in the background.
  2. pyocd pulls SWV/ITM data off the ST-Link and exposes it on a TCP port.
  3. We connect to that port, parse ITM stimulus-port 0 bytes into lines,
     and drive a customtkinter UI.

Setup once:
    pip install pyocd customtkinter

Run:
    python shazam_monitor.py

IMPORTANT: Close the CubeIDE debug session before running this. Only one
process at a time can own the ST-Link.
"""

import os
import re
import sys
import socket
import shutil
import signal
import subprocess
import threading
import queue
import time
import atexit

import customtkinter as ctk

# ---------------------------------------------------------- config
TARGET           = "stm32f407vg"
CPU_CLOCK_HZ     = 16_000_000        # must match HCLK in SystemClock_Config
SWO_CLOCK_HZ     = 2_000_000
SWV_TCP_PORT     = 3443
# Prefer `python -m pyocd` so we use the same interpreter that launched us —
# avoids PATH issues where pip-installed scripts aren't on the shell's PATH.
PYOCD_CMD        = os.environ.get("PYOCD", shutil.which("pyocd"))

SONGS = {
    0: ("Sinewave Anthem",       "100 / 300 / 600 / 1200 Hz  — matches DB song 1"),
    1: ("Odd Harmonics",         "150 / 450 / 900 / 1800 Hz  — decoy"),
    2: ("Crystal Chords",        "125 / 375 / 625 / 1125 Hz  — matches DB song 2"),
    3: ("Just Intonation Blues", "220 / 440 / 880 / 1760 Hz  — decoy"),
}

RE_PROC   = re.compile(r"^PROCESSING\s+(\d+)")
RE_RESULT = re.compile(
    r"^RESULT\s+idx=(\d+)\s+song=(\d+)\s+votes=(\d+)\s+conf=([0-9.]+)"
)

# ---------------------------------------------------------- pyocd launcher
def launch_pyocd() -> subprocess.Popen:
    # Use `python -m pyocd` when pyocd isn't on PATH, so we work in environments
    # where pip-installed scripts didn't get added to the shell's PATH.
    if PYOCD_CMD:
        args = [PYOCD_CMD]
    else:
        args = [sys.executable, "-m", "pyocd"]
    args += [
        "gdbserver",
        "--target", TARGET,
        "--persist",
        # Reset & run so the target isn't sitting halted waiting for a GDB
        # client to connect — gdbserver's default is to halt on launch.
        "--reset-run",
        # Don't halt on hard faults or other vector catches — we're a
        # demo, not a debug session; keep the firmware running even if
        # something misbehaves so we can see the ITM output.
        "-C", "none",
        "-bh",
        "-O", "enable_swv=True",
        "-O", "swv_raw_enable=True",
        "-O", f"swv_raw_port={SWV_TCP_PORT}",
        "-O", f"swv_clock={SWO_CLOCK_HZ}",
        "-O", f"swv_system_clock={CPU_CLOCK_HZ}",
    ]
    try:
        return subprocess.Popen(
            args,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            creationflags=(subprocess.CREATE_NEW_PROCESS_GROUP
                           if os.name == "nt" else 0),
        )
    except FileNotFoundError:
        print("pyocd not found. Install with: pip install pyocd", file=sys.stderr)
        sys.exit(1)

# ---------------------------------------------------------- ITM parser
def itm_reader(sock: socket.socket, q: queue.Queue):
    """Parse ITM frames from `sock` and push ASCII lines (stimulus port 0)
    into `q` as ("line", <str>) events."""
    line = bytearray()
    buf  = bytearray()

    def read_one() -> int | None:
        nonlocal buf
        while not buf:
            try:
                chunk = sock.recv(4096)
            except OSError:
                return None
            if not chunk:
                return None
            buf.extend(chunk)
        b = buf[0]
        del buf[0]
        return b

    while True:
        hdr = read_one()
        if hdr is None:
            q.put(("status", "SWV socket closed"))
            return

        # Sync / overflow
        if hdr == 0x00 or hdr == 0x70:
            continue

        # Local / global timestamp packets — discard continuation bytes (MSB=1)
        if (hdr & 0x0F) == 0x00 and (hdr & 0xC0) == 0xC0:
            while True:
                nxt = read_one()
                if nxt is None: return
                if (nxt & 0x80) == 0:
                    break
            continue

        # SWIT / stimulus port instrumentation packets
        # header = (port << 3) | size   where size in {0b01,0b10,0b11} (1,2,4 bytes)
        size_code = hdr & 0x03
        if size_code == 0:
            # protocol packet we don't care about — skip 1 byte best-effort
            continue

        port = (hdr >> 3) & 0x1F
        nbytes = {1: 1, 2: 2, 3: 4}[size_code]
        payload = bytearray()
        for _ in range(nbytes):
            b = read_one()
            if b is None: return
            payload.append(b)

        if port != 0:
            continue

        for ch in payload:
            if ch == 0x0A:          # \n
                try:
                    s = line.decode("ascii", errors="ignore").rstrip()
                except Exception:
                    s = ""
                if s:
                    q.put(("line", s))
                line.clear()
            elif ch == 0x0D:        # \r
                pass
            elif 0x20 <= ch < 0x7F:
                line.append(ch)


def swv_connect_thread(q: queue.Queue):
    """Wait for pyocd to open its SWV port, then stream ITM bytes forever."""
    # Retry for ~60s — pyocd can sit waiting for a probe on slow USB enumeration.
    for i in range(120):
        try:
            s = socket.create_connection(("127.0.0.1", SWV_TCP_PORT), timeout=1.5)
            q.put(("status", f"connected to pyocd SWV on :{SWV_TCP_PORT}"))
            itm_reader(s, q)
            return
        except OSError:
            if i == 6:
                q.put(("status", "waiting for ST-Link… plug in the board & close CubeIDE debug"))
            time.sleep(0.5)
    q.put(("status", "could not connect to pyocd SWV port — is the ST-Link plugged in?"))

# ---------------------------------------------------------- UI
class App(ctk.CTk):
    def __init__(self, q: queue.Queue):
        super().__init__()
        ctk.set_appearance_mode("dark")
        ctk.set_default_color_theme("blue")
        self.title("STM32 Shazam Monitor (SWV)")
        self.geometry("580x400")
        self.minsize(480, 360)

        self.status_var = ctk.StringVar(value="starting pyocd…")
        self.title_var  = ctk.StringVar(value="—")
        self.artist_var = ctk.StringVar(value="press the blue button on the STM32")
        self.meta_var   = ctk.StringVar(value="")

        ctk.CTkLabel(self, text="Now identifying", font=("Segoe UI", 14),
                     text_color="#9aa").pack(pady=(30, 2))
        ctk.CTkLabel(self, textvariable=self.title_var,
                     font=("Segoe UI Semibold", 30)).pack()
        ctk.CTkLabel(self, textvariable=self.artist_var,
                     font=("Segoe UI", 16), text_color="#bbb").pack(pady=(0, 18))

        self.pill = ctk.CTkLabel(self, text="IDLE", corner_radius=14,
                                 fg_color="#333", width=180, height=36,
                                 font=("Segoe UI Semibold", 14))
        self.pill.pack(pady=4)

        ctk.CTkLabel(self, textvariable=self.meta_var,
                     font=("Consolas", 11), text_color="#888").pack(pady=(20, 0))
        ctk.CTkLabel(self, textvariable=self.status_var,
                     font=("Consolas", 10), text_color="#555").pack(side="bottom", pady=8)

        self.q = q
        self.after(50, self._drain)

    def _drain(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "status":
                    self.status_var.set(payload)
                elif kind == "line":
                    self._handle_line(payload)
        except queue.Empty:
            pass
        self.after(50, self._drain)

    def _handle_line(self, line: str):
        self.status_var.set(line)

        if line == "BOOT":
            self._set_pill("READY", "#1f6aa5")
            self.title_var.set("Press the blue button")
            self.artist_var.set("on the STM32F407 Discovery board")
            self.meta_var.set("")
            return

        m = RE_PROC.match(line)
        if m:
            idx = int(m.group(1))
            t, a = SONGS.get(idx, ("unknown", "?"))
            self._set_pill("IDENTIFYING…", "#b07d00")
            self.title_var.set(t)
            self.artist_var.set(a)
            self.meta_var.set(f"idx={idx}   (running FFT + hash + match)")
            return

        m = RE_RESULT.match(line)
        if m:
            idx, song, votes, conf = m.groups()
            votes_i = int(votes); conf_f = float(conf)
            if votes_i > 0:
                self._set_pill("MATCHED", "#1f8a3d")
            else:
                self._set_pill("NO MATCH", "#8a1f1f")
            self.meta_var.set(
                f"idx={idx}   db_song_id={song}   votes={votes_i}   confidence={conf_f:.3f}"
            )
            return

    def _set_pill(self, text: str, color: str):
        self.pill.configure(text=text, fg_color=color)

# ---------------------------------------------------------- main
def main():
    q: queue.Queue = queue.Queue()

    pyocd_proc = launch_pyocd()

    def cleanup():
        try:
            if pyocd_proc.poll() is None:
                if os.name == "nt":
                    pyocd_proc.send_signal(signal.CTRL_BREAK_EVENT)
                else:
                    pyocd_proc.terminate()
                try:
                    pyocd_proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    pyocd_proc.kill()
        except Exception:
            pass
    atexit.register(cleanup)

    q.put(("status", "pyocd starting…"))
    threading.Thread(target=swv_connect_thread, args=(q,), daemon=True).start()

    # Also drain pyocd stderr so it doesn't block and so we can show its errors.
    def drain_stderr():
        for raw in pyocd_proc.stderr:
            try:
                line = raw.decode("utf-8", errors="ignore").rstrip()
            except Exception:
                continue
            if line:
                # Surface pyocd warnings/errors in the status bar
                q.put(("status", f"pyocd: {line}"))
    threading.Thread(target=drain_stderr, daemon=True).start()

    App(q).mainloop()

if __name__ == "__main__":
    main()
