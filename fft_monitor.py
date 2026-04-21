"""
fft_monitor.py  —  STM32 FFT Spectrum Analyzer UI over SWV/ITM.

No extra hardware. Uses the ST-Link cable already connected.

SWV protocol (from firmware):
    BOOT                                 board ready
    FFT_START sig=<n>                    starting analysis on signal n
    FFT_FRAME <frame> <b0> ... <b63>    64 uint8 magnitude bins
    FFT_DONE sig=<n>                     all frames sent

Setup (once):
    pip install pyocd customtkinter matplotlib numpy

Run:
    python fft_monitor.py

IMPORTANT: Close the CubeIDE debug session first — only one process at a
time can own the ST-Link.
"""

import os, re, sys, socket, shutil, signal, subprocess
import threading, queue, time, atexit

import numpy as np
import customtkinter as ctk
import tkinter as tk
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
import matplotlib.colors as mcolors

# ------------------------------------------------------------------ config
TARGET       = "stm32f407vg"
CPU_CLOCK_HZ = 16_000_000
SWO_CLOCK_HZ = 2_000_000
SWV_TCP_PORT = 3443
PYOCD_CMD    = os.environ.get("PYOCD", shutil.which("pyocd"))

SAMPLE_RATE  = 8000
FFT_SIZE     = 1024
REPORT_BINS  = 64
BIN_HZ       = SAMPLE_RATE / FFT_SIZE   # ~7.81 Hz per bin

SIGNAL_NAMES = {
    0: ("Signal A", "100 / 300 / 600 / 1200 Hz"),
    1: ("Signal B", "150 / 450 / 900 / 1800 Hz"),
    2: ("Signal C", "125 / 375 / 625 / 1125 Hz"),
    3: ("Signal D", "220 / 440 / 880 / 1760 Hz"),
}

# Colour stops for the spectrum bars (magnitude 0→255)
BAR_CMAP = mcolors.LinearSegmentedColormap.from_list(
    "spectrum", ["#0a2342", "#0077b6", "#00b4d8", "#90e0ef", "#caf0f8"], N=256
)

RE_START = re.compile(r"^FFT_START\s+sig=(\d+)")
RE_FRAME = re.compile(r"^FFT_FRAME\s+(\d+)\s+([\d ]+)")
RE_DONE  = re.compile(r"^FFT_DONE\s+sig=(\d+)")

# ------------------------------------------------------------------ pyocd
def launch_pyocd() -> subprocess.Popen:
    if PYOCD_CMD:
        args = [PYOCD_CMD]
    else:
        args = [sys.executable, "-m", "pyocd"]
    args += [
        "gdbserver",
        "--target", TARGET,
        "--persist",
        "--reset-run",
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
        print("pyocd not found — install with: pip install pyocd", file=sys.stderr)
        sys.exit(1)

# ------------------------------------------------------------------ ITM parser
def itm_reader(sock: socket.socket, q: queue.Queue):
    """Parse ITM frames from sock; push ('line', str) events into q."""
    line = bytearray()
    buf  = bytearray()

    def read_one():
        nonlocal buf
        while not buf:
            try:
                chunk = sock.recv(4096)
            except OSError:
                return None
            if not chunk:
                return None
            buf.extend(chunk)
        b = buf[0]; del buf[0]; return b

    while True:
        hdr = read_one()
        if hdr is None:
            q.put(("status", "SWV socket closed")); return

        if hdr == 0x00 or hdr == 0x70:
            continue
        if (hdr & 0x0F) == 0x00 and (hdr & 0xC0) == 0xC0:
            while True:
                nxt = read_one()
                if nxt is None: return
                if (nxt & 0x80) == 0: break
            continue

        size_code = hdr & 0x03
        if size_code == 0:
            continue
        port   = (hdr >> 3) & 0x1F
        nbytes = {1: 1, 2: 2, 3: 4}[size_code]
        payload = bytearray()
        for _ in range(nbytes):
            b = read_one()
            if b is None: return
            payload.append(b)

        if port != 0:
            continue

        for ch in payload:
            if ch == 0x0A:
                try:
                    s = line.decode("ascii", errors="ignore").rstrip()
                except Exception:
                    s = ""
                if s:
                    q.put(("line", s))
                line.clear()
            elif ch == 0x0D:
                pass
            elif 0x20 <= ch < 0x7F:
                line.append(ch)

def swv_connect_thread(q: queue.Queue):
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
    q.put(("status", "could not connect to pyocd — is the ST-Link plugged in?"))

# ------------------------------------------------------------------ UI
DARK_BG    = "#080d14"
PANEL_BG   = "#0d1520"
ACCENT     = "#00b4d8"
ACCENT2    = "#7b2ff7"
TEXT_DIM   = "#4a6080"
TEXT_MID   = "#8fa8c8"
TEXT_BRIGHT= "#e0eeff"

class App(ctk.CTk):
    BINS       = REPORT_BINS
    PEAK_DECAY = 8   # frames before peak hold drops by 1 unit

    def __init__(self, q: queue.Queue):
        super().__init__()
        ctk.set_appearance_mode("dark")
        ctk.set_default_color_theme("blue")

        self.title("STM32 FFT Spectrum Analyzer")
        self.geometry("960x620")
        self.minsize(760, 500)
        self.configure(fg_color=DARK_BG)

        self._mags       = np.zeros(self.BINS)
        self._peaks      = np.zeros(self.BINS)
        self._peak_age   = np.zeros(self.BINS, dtype=int)
        self._frame_no   = 0
        self._total_frames = 0
        self._running    = False

        self._build_ui()
        self.q = q
        self.after(50, self._drain)

    # ---- layout ----------------------------------------------------------
    def _build_ui(self):
        self._build_header()
        self._build_info_bar()
        self._build_chart()
        self._build_footer()

    def _build_header(self):
        hdr = ctk.CTkFrame(self, fg_color=PANEL_BG, corner_radius=0, height=64)
        hdr.pack(fill="x")
        hdr.pack_propagate(False)

        # left: icon + title
        left = ctk.CTkFrame(hdr, fg_color="transparent")
        left.pack(side="left", padx=20, pady=10)
        ctk.CTkLabel(left, text="◈", font=("Segoe UI", 26),
                     text_color=ACCENT).pack(side="left", padx=(0, 8))
        ctk.CTkLabel(left, text="FFT SPECTRUM ANALYZER",
                     font=("Segoe UI Bold", 18),
                     text_color=TEXT_BRIGHT).pack(side="left")
        ctk.CTkLabel(left, text="  STM32F407 · SWV/ITM",
                     font=("Segoe UI", 11),
                     text_color=TEXT_DIM).pack(side="left", pady=(4, 0))

        # right: status pill
        self._pill = ctk.CTkLabel(
            hdr, text="IDLE", corner_radius=10,
            fg_color="#1a1a2e", width=130, height=34,
            font=("Segoe UI Semibold", 13), text_color=TEXT_DIM)
        self._pill.pack(side="right", padx=20, pady=15)

    def _build_info_bar(self):
        bar = ctk.CTkFrame(self, fg_color="#0a1018", corner_radius=0, height=38)
        bar.pack(fill="x")
        bar.pack_propagate(False)

        self._sig_name_var = ctk.StringVar(value="—")
        self._sig_freq_var = ctk.StringVar(value="Press the blue button on the STM32 board")
        self._frame_var    = ctk.StringVar(value="")

        ctk.CTkLabel(bar, textvariable=self._sig_name_var,
                     font=("Segoe UI Semibold", 13), text_color=ACCENT
                     ).pack(side="left", padx=(18, 6), pady=9)
        ctk.CTkLabel(bar, textvariable=self._sig_freq_var,
                     font=("Segoe UI", 12), text_color=TEXT_MID
                     ).pack(side="left", padx=0, pady=9)
        ctk.CTkLabel(bar, textvariable=self._frame_var,
                     font=("Consolas", 11), text_color=TEXT_DIM
                     ).pack(side="right", padx=18, pady=9)

    def _build_chart(self):
        outer = ctk.CTkFrame(self, fg_color=PANEL_BG, corner_radius=14)
        outer.pack(fill="both", expand=True, padx=14, pady=(8, 4))

        # matplotlib figure
        self._fig = Figure(figsize=(10, 4), facecolor=PANEL_BG, dpi=100)
        self._ax  = self._fig.add_subplot(111)
        self._ax.set_facecolor("#0a1018")
        self._fig.subplots_adjust(left=0.06, right=0.99, top=0.90, bottom=0.16)

        x = np.arange(self.BINS)

        # bars — coloured by bin position initially
        init_colors = [BAR_CMAP(b / self.BINS) for b in range(self.BINS)]
        self._bar_container = self._ax.bar(
            x, np.zeros(self.BINS), width=0.85,
            color=init_colors, zorder=3)

        # peak-hold line
        self._peak_line, = self._ax.plot(
            x, np.zeros(self.BINS),
            color="#ff6b6b", linewidth=1.5,
            linestyle="--", alpha=0.8, zorder=4)

        # axes cosmetics
        self._ax.set_xlim(-0.6, self.BINS - 0.4)
        self._ax.set_ylim(0, 270)

        tick_pos = list(range(0, self.BINS, 8))
        self._ax.set_xticks(tick_pos)
        self._ax.set_xticklabels(
            [f"{int(p * BIN_HZ)}" for p in tick_pos],
            color=TEXT_DIM, fontsize=8)
        self._ax.set_yticks([0, 64, 128, 192, 255])
        self._ax.set_yticklabels(["0", "64", "128", "192", "255"],
                                  color=TEXT_DIM, fontsize=8)

        self._ax.set_xlabel("Frequency (Hz)",
                            color=TEXT_MID, fontsize=9, labelpad=4)
        self._ax.set_ylabel("Magnitude",
                            color=TEXT_MID, fontsize=9, labelpad=4)
        self._ax.set_title("Waiting for STM32…",
                           color=TEXT_MID, fontsize=10, pad=8)

        self._ax.tick_params(colors=TEXT_DIM, length=3)
        for spine in self._ax.spines.values():
            spine.set_edgecolor("#1a2840")
        self._ax.grid(axis="y", color="#1a2840", linewidth=0.6, zorder=1)
        self._ax.grid(axis="x", color="#111d2e", linewidth=0.4, zorder=1)

        # embed canvas
        self._canvas = FigureCanvasTkAgg(self._fig, master=outer)
        self._canvas.get_tk_widget().pack(fill="both", expand=True,
                                          padx=6, pady=6)
        self._canvas.draw()

    def _build_footer(self):
        foot = ctk.CTkFrame(self, fg_color=PANEL_BG, corner_radius=0, height=30)
        foot.pack(fill="x", side="bottom")
        foot.pack_propagate(False)

        self._status_var = ctk.StringVar(value="starting pyocd…")
        ctk.CTkLabel(foot, textvariable=self._status_var,
                     font=("Consolas", 10), text_color=TEXT_DIM
                     ).pack(side="left", padx=14, pady=6)

        ctk.CTkLabel(foot, text=f"fs={SAMPLE_RATE} Hz  |  FFT={FFT_SIZE}  |  "
                                f"bins={REPORT_BINS}  |  Δf={BIN_HZ:.2f} Hz/bin",
                     font=("Consolas", 10), text_color=TEXT_DIM
                     ).pack(side="right", padx=14, pady=6)

    # ---- queue drain -----------------------------------------------------
    def _drain(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "status":
                    self._status_var.set(payload)
                elif kind == "line":
                    self._handle_line(payload)
        except queue.Empty:
            pass
        self.after(50, self._drain)

    # ---- line handler ----------------------------------------------------
    def _handle_line(self, line: str):
        self._status_var.set(line)

        if line == "BOOT":
            self._set_pill("READY", ACCENT, TEXT_BRIGHT)
            self._ax.set_title("Ready — press the blue button",
                               color=TEXT_MID, fontsize=10)
            self._canvas.draw_idle()
            return

        m = RE_START.match(line)
        if m:
            idx = int(m.group(1))
            name, freqs = SIGNAL_NAMES.get(idx, ("Signal ?", ""))
            self._running   = True
            self._frame_no  = 0
            self._mags[:]   = 0
            self._peaks[:]  = 0
            self._peak_age[:] = 0
            self._sig_name_var.set(name)
            self._sig_freq_var.set(f"  {freqs}")
            self._frame_var.set("Frame 0")
            self._set_pill("COMPUTING…", "#b07d00", "#ffe066")
            self._ax.set_title(f"{name}  ·  {freqs}",
                               color=ACCENT, fontsize=10)
            self._canvas.draw_idle()
            return

        m = RE_FRAME.match(line)
        if m:
            self._frame_no += 1
            vals = list(map(int, m.group(2).split()))
            n = min(len(vals), self.BINS)
            self._mags[:n] = vals[:n]

            # update peak hold
            for b in range(self.BINS):
                if self._mags[b] >= self._peaks[b]:
                    self._peaks[b]    = self._mags[b]
                    self._peak_age[b] = 0
                else:
                    self._peak_age[b] += 1
                    if self._peak_age[b] > self.PEAK_DECAY:
                        self._peaks[b] = max(0, self._peaks[b] - 4)

            self._update_chart()
            self._frame_var.set(f"Frame {self._frame_no}")
            return

        m = RE_DONE.match(line)
        if m:
            self._running = False
            self._set_pill("DONE ✓", "#1d8a3d", "#80ffb0")
            self._total_frames = self._frame_no
            self._frame_var.set(
                f"Frame {self._total_frames} / {self._total_frames}  ·  done")
            return

    # ---- chart update ----------------------------------------------------
    def _update_chart(self):
        norm = self._mags / 255.0          # 0.0 – 1.0
        colors = [BAR_CMAP(float(v)) for v in norm]
        for bar, h, c in zip(self._bar_container, self._mags, colors):
            bar.set_height(float(h))
            bar.set_color(c)
        self._peak_line.set_ydata(self._peaks)
        self._canvas.draw_idle()

    # ---- pill helper -----------------------------------------------------
    def _set_pill(self, text: str, fg: str, txt: str = TEXT_BRIGHT):
        self._pill.configure(text=text, fg_color=fg, text_color=txt)


# ------------------------------------------------------------------ main
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

    def drain_stderr():
        for raw in pyocd_proc.stderr:
            try:
                ln = raw.decode("utf-8", errors="ignore").rstrip()
            except Exception:
                continue
            if ln:
                q.put(("status", f"pyocd: {ln}"))
    threading.Thread(target=drain_stderr, daemon=True).start()

    App(q).mainloop()


if __name__ == "__main__":
    main()
