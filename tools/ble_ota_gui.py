#!/usr/bin/env python3
"""
BLE OTA GUI updater for StarLineBLE (ESP32).
Modern dark UI with rounded, glass-like panels (built with tkinter Canvas,
no external UI dependencies).

Requires:
    pip install bleak
"""
import asyncio
import sys
import time
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from pathlib import Path
import subprocess

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    messagebox.showerror("Error", "bleak not installed.\nRun: pip install bleak")
    sys.exit(1)

# Must match src/ble_ota.h
OTA_SERVICE_UUID = "daf177f0-6f7a-4f5a-8f4a-4a3a2a1a0a01"
OTA_DATA_UUID = "daf177f1-6f7a-4f5a-8f4a-4a3a2a1a0a02"
OTA_CTRL_UUID = "daf177f2-6f7a-4f5a-8f4a-4a3a2a1a0a03"

CHUNK_SIZE = 512

# ----------------------------------------------------------------------------
# Theme
# ----------------------------------------------------------------------------
COLORS = {
    "bg": "#0a0b10",
    "card": "#14161f",
    "input_bg": "#1c1f2e",
    "border": "#262a3d",
    "accent": "#7c6cff",
    "accent_hover": "#9285ff",
    "accent2": "#22d3ee",
    "text": "#eef0fa",
    "text_dim": "#8b8fa8",
    "success": "#34d399",
    "danger": "#f87171",
    "warning": "#fbbf24",
}
FONT_BASE = "Segoe UI"


def round_rect_points(x1, y1, x2, y2, radius):
    """Return a point list describing a rounded rectangle for create_polygon(smooth=True)."""
    r = min(radius, (x2 - x1) / 2, (y2 - y1) / 2)
    return [
        x1 + r, y1,
        x2 - r, y1,
        x2, y1,
        x2, y1 + r,
        x2, y2 - r,
        x2, y2,
        x2 - r, y2,
        x1 + r, y2,
        x1, y2,
        x1, y2 - r,
        x1, y1 + r,
        x1, y1,
    ]


class RoundedButton(tk.Canvas):
    """A flat, rounded-corner clickable button drawn on a Canvas."""

    def __init__(self, parent, text, command=None, width=140, height=40, radius=14,
                 bg=COLORS["input_bg"], fg=COLORS["text"], hover_bg=None, active_bg=None,
                 disabled_bg="#15161d", font=None, parent_bg=None):
        parent_bg = parent_bg or parent.cget("bg")
        super().__init__(parent, width=width, height=height, bg=parent_bg,
                          highlightthickness=0, bd=0)
        self.command = command
        self.width, self.height, self.radius = width, height, radius
        self.bg_color = bg
        self.hover_color = hover_bg or bg
        self.active_color = active_bg or bg
        self.disabled_color = disabled_bg
        self.fg_color = fg
        self.text = text
        self.font = font or (FONT_BASE, 10, "bold")
        self.enabled = True

        self._draw(self.bg_color)
        self.bind("<Enter>", self._on_enter)
        self.bind("<Leave>", self._on_leave)
        self.bind("<ButtonPress-1>", self._on_press)
        self.bind("<ButtonRelease-1>", self._on_release)

    def _draw(self, fill):
        self.delete("all")
        pts = round_rect_points(1, 1, self.width - 1, self.height - 1, self.radius)
        self.create_polygon(pts, smooth=True, fill=fill, outline="")
        text_color = self.fg_color if self.enabled else "#565a6e"
        self.create_text(self.width // 2, self.height // 2, text=self.text,
                          fill=text_color, font=self.font)

    def _on_enter(self, _e):
        if self.enabled:
            self._draw(self.hover_color)
            self.config(cursor="hand2")

    def _on_leave(self, _e):
        self._draw(self.bg_color if self.enabled else self.disabled_color)

    def _on_press(self, _e):
        if self.enabled:
            self._draw(self.active_color)

    def _on_release(self, _e):
        if self.enabled:
            self._draw(self.hover_color)
            if self.command:
                self.command()

    def set_enabled(self, enabled: bool):
        self.enabled = enabled
        self.config(cursor="hand2" if enabled else "arrow")
        self._draw(self.bg_color if enabled else self.disabled_color)


class ToggleSwitch(tk.Canvas):
    """A modern on/off pill switch bound to a tk.BooleanVar."""

    def __init__(self, parent, variable, command=None, width=44, height=24,
                 on_color=COLORS["accent"], off_color=COLORS["border"],
                 knob_color="#ffffff", parent_bg=None):
        parent_bg = parent_bg or parent.cget("bg")
        super().__init__(parent, width=width, height=height, bg=parent_bg,
                          highlightthickness=0, bd=0, cursor="hand2")
        self.variable = variable
        self.command = command
        self.w, self.h = width, height
        self.on_color, self.off_color, self.knob_color = on_color, off_color, knob_color
        self.bind("<Button-1>", self._toggle)
        self._draw()

    def _draw(self):
        self.delete("all")
        on = self.variable.get()
        track_color = self.on_color if on else self.off_color
        pts = round_rect_points(1, 1, self.w - 1, self.h - 1, self.h // 2 - 1)
        self.create_polygon(pts, smooth=True, fill=track_color, outline="")
        knob_r = self.h // 2 - 3
        cx = self.w - self.h // 2 if on else self.h // 2
        cy = self.h // 2
        self.create_oval(cx - knob_r, cy - knob_r, cx + knob_r, cy + knob_r,
                          fill=self.knob_color, outline="")

    def _toggle(self, _e):
        self.variable.set(not self.variable.get())
        self._draw()
        if self.command:
            self.command()


class RoundedProgress(tk.Canvas):
    """A rounded progress bar with determinate + animated indeterminate modes."""

    def __init__(self, parent, width=380, height=14, radius=7,
                 track_color=COLORS["input_bg"], fill_color=COLORS["accent2"],
                 parent_bg=None):
        parent_bg = parent_bg or parent.cget("bg")
        super().__init__(parent, width=width, height=height, bg=parent_bg,
                          highlightthickness=0, bd=0)
        self.w, self.h, self.radius = width, height, radius
        self.track_color = track_color
        self.fill_color = fill_color
        self.value = 0.0
        self.indeterminate = False
        self._anim_pos = 0
        self._anim_job = None
        self._draw_track()

    def _draw_track(self):
        self.delete("track")
        pts = round_rect_points(0, 0, self.w, self.h, self.radius)
        self.create_polygon(pts, smooth=True, fill=self.track_color, outline="", tags="track")

    def set_value(self, percent):
        self.value = max(0.0, min(100.0, percent))
        self.delete("fill")
        fw = int(self.w * (self.value / 100.0))
        if fw > 2:
            pts = round_rect_points(0, 0, fw, self.h, self.radius)
            self.create_polygon(pts, smooth=True, fill=self.fill_color, outline="", tags="fill")

    def start_indeterminate(self):
        if self.indeterminate:
            return
        self.indeterminate = True
        self._anim_pos = -self.w // 4
        self._animate()

    def stop_indeterminate(self):
        self.indeterminate = False
        if self._anim_job:
            self.after_cancel(self._anim_job)
            self._anim_job = None
        self.delete("fill")

    def _animate(self):
        if not self.indeterminate:
            return
        self.delete("fill")
        seg_w = max(40, self.w // 4)
        x1 = self._anim_pos
        x2 = min(self.w, x1 + seg_w)
        if x2 > 0:
            pts = round_rect_points(max(0, x1), 0, x2, self.h, self.radius)
            self.create_polygon(pts, smooth=True, fill=self.fill_color, outline="", tags="fill")
        self._anim_pos += 10
        if self._anim_pos > self.w:
            self._anim_pos = -seg_w
        self._anim_job = self.after(30, self._animate)


class BleOtaGui:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("StarLineBLE OTA")
        self.root.resizable(False, False)

        self.firmware_path = tk.StringVar()
        self.status = tk.StringVar(value="Ready")
        self.progress = tk.DoubleVar(value=0.0)
        self.build_release = tk.BooleanVar(value=True)
        self.devices = []
        self.selected_address = None
        self._build_result = None
        self._build_done = threading.Event()

        self._build_ui()

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------
    def _section_label(self, parent, text):
        tk.Label(parent, text=text, bg=COLORS["card"], fg=COLORS["text_dim"],
                 font=(FONT_BASE, 8, "bold")).pack(anchor="w", pady=(2, 0))

    def _rounded_entry_wrap(self, parent, width, height=40, radius=12):
        wrap = tk.Canvas(parent, width=width, height=height, bg=COLORS["card"],
                          highlightthickness=0, bd=0)
        pts = round_rect_points(1, 1, width - 1, height - 1, radius)
        wrap.create_polygon(pts, smooth=True, fill=COLORS["input_bg"], outline=COLORS["border"])
        return wrap

    def _build_ui(self):
        C = COLORS
        self.root.configure(bg=C["bg"])

        outer = tk.Frame(self.root, bg=C["bg"])
        outer.pack(padx=22, pady=22)

        CARD_W, CARD_H = 440, 600
        RADIUS = 22
        card_canvas = tk.Canvas(outer, width=CARD_W, height=CARD_H, bg=C["bg"],
                                 highlightthickness=0, bd=0)
        card_canvas.pack()
        pts = round_rect_points(1, 1, CARD_W - 1, CARD_H - 1, RADIUS)
        card_canvas.create_polygon(pts, smooth=True, fill=C["card"], outline=C["border"], width=1)

        inset = RADIUS + 6
        inner = tk.Frame(card_canvas, bg=C["card"])
        card_canvas.create_window(CARD_W // 2, CARD_H // 2, window=inner,
                                   width=CARD_W - 2 * inset, height=CARD_H - 2 * inset)
        content_w = CARD_W - 2 * inset

        # Header
        header = tk.Frame(inner, bg=C["card"])
        header.pack(fill="x", pady=(0, 20))
        tk.Label(header, text="\u26a1", font=(FONT_BASE, 20), bg=C["card"],
                 fg=C["accent2"]).pack(side="left")
        tk.Label(header, text="StarLineBLE OTA", font=(FONT_BASE, 16, "bold"),
                 bg=C["card"], fg=C["text"]).pack(side="left", padx=(8, 0))

        # Firmware section
        self._section_label(inner, "FIRMWARE")
        fw_row = tk.Frame(inner, bg=C["card"])
        fw_row.pack(fill="x", pady=(6, 10))
        entry_w = content_w - 100
        fw_wrap = self._rounded_entry_wrap(fw_row, width=entry_w)
        fw_wrap.pack(side="left")
        self.fw_entry = tk.Entry(fw_wrap, textvariable=self.firmware_path,
                                   bg=C["input_bg"], fg=C["text"], insertbackground=C["text"],
                                   disabledbackground=C["input_bg"], disabledforeground=C["text_dim"],
                                   relief="flat", bd=0, font=(FONT_BASE, 10))
        self.fw_entry.place(x=10, y=8, width=entry_w - 20, height=24)
        self.browse_btn = RoundedButton(
            fw_row, "Browse", command=self._browse, width=90, height=40, radius=12,
            bg=C["input_bg"], hover_bg=C["border"], active_bg=C["border"],
            fg=C["text"], font=(FONT_BASE, 9, "bold"), parent_bg=C["card"])
        self.browse_btn.pack(side="left", padx=(10, 0))

        # Build toggle (under firmware field)
        build_row = tk.Frame(inner, bg=C["card"])
        build_row.pack(fill="x", pady=(0, 16))
        self.build_toggle = ToggleSwitch(
            build_row, self.build_release, command=self._on_build_toggle,
            on_color=C["accent"], off_color=C["border"], knob_color="#ffffff",
            parent_bg=C["card"])
        self.build_toggle.pack(side="left")
        tk.Label(build_row, text="Build release before OTA", bg=C["card"],
                 fg=C["text_dim"], font=(FONT_BASE, 10)).pack(side="left", padx=(10, 0))

        # Devices section
        self._section_label(inner, "DEVICES")
        list_wrap = tk.Frame(inner, bg=C["input_bg"], highlightthickness=1,
                              highlightbackground=C["border"], highlightcolor=C["accent"])
        list_wrap.pack(fill="x", pady=(6, 10))
        self.lst = tk.Listbox(list_wrap, height=4, bg=C["input_bg"], fg=C["text"],
                               selectbackground=C["accent"], selectforeground="#ffffff",
                               activestyle="none", relief="flat", bd=0,
                               highlightthickness=0, font=(FONT_BASE, 10))
        self.lst.pack(fill="both", expand=True, padx=6, pady=6)
        self.lst.bind("<<ListboxSelect>>", self._on_select)

        scan_row = tk.Frame(inner, bg=C["card"])
        scan_row.pack(fill="x", pady=(0, 10))
        self.scan_btn = RoundedButton(
            scan_row, "\U0001F50D  Scan for Devices", command=self._scan,
            width=200, height=40, radius=12,
            bg=C["input_bg"], hover_bg=C["border"], active_bg=C["border"],
            fg=C["text"], font=(FONT_BASE, 10, "bold"), parent_bg=C["card"])
        self.scan_btn.pack(side="left")

        # Scan progress indicator
        self.scan_progress = RoundedProgress(
            scan_row, width=100, height=14, radius=7,
            track_color=C["input_bg"], fill_color=C["warning"], parent_bg=C["card"])
        self.scan_progress.pack(side="left", padx=(10, 0))
        self.scan_progress.pack_forget()  # Hidden by default

        # Progress
        self._section_label(inner, "PROGRESS")
        self.progressbar = RoundedProgress(
            inner, width=content_w, height=14, radius=7,
            track_color=C["input_bg"], fill_color=C["accent2"], parent_bg=C["card"])
        self.progressbar.pack(pady=(6, 20))
        self.progress.trace_add("write", lambda *a: self.progressbar.set_value(self.progress.get()))

        # Action buttons
        action_row = tk.Frame(inner, bg=C["card"])
        action_row.pack(fill="x", pady=(0, 16))
        self.start_btn = RoundedButton(
            action_row, "\u25b6  Start OTA", command=self._start_ota,
            width=content_w - 100, height=44, radius=14,
            bg=C["accent"], hover_bg=C["accent_hover"], active_bg=C["accent"],
            fg="#ffffff", font=(FONT_BASE, 11, "bold"), parent_bg=C["card"])
        self.start_btn.pack(side="left")
        quit_btn = RoundedButton(
            action_row, "Quit", command=self.root.quit, width=90, height=44, radius=14,
            bg=C["input_bg"], hover_bg=C["border"], active_bg=C["border"],
            fg=C["text_dim"], font=(FONT_BASE, 11), parent_bg=C["card"])
        quit_btn.pack(side="right")

        # Status
        status_row = tk.Frame(inner, bg=C["card"])
        status_row.pack(fill="x", pady=(6, 0))
        self.status_dot = tk.Canvas(status_row, width=10, height=10, bg=C["card"],
                                     highlightthickness=0)
        self.status_dot.pack(side="left")
        self._dot_id = self.status_dot.create_oval(1, 1, 9, 9, fill=C["text_dim"], outline="")
        tk.Label(status_row, textvariable=self.status, bg=C["card"], fg=C["text_dim"],
                 font=(FONT_BASE, 9), anchor="w", justify="left",
                 wraplength=content_w - 24).pack(side="left", padx=(8, 0), fill="x")
        self.status.trace_add("write", self._update_status_dot)

        self._on_build_toggle()

    def _update_status_dot(self, *_args):
        text = self.status.get().lower()
        C = COLORS
        if "error" in text or "fail" in text:
            color = C["danger"]
        elif "done" in text or "success" in text:
            color = C["success"]
        elif any(k in text for k in ("progress", "scan", "connect", "building", "start", "pair")):
            color = C["warning"]
        else:
            color = C["text_dim"]
        self.status_dot.itemconfig(self._dot_id, fill=color)

    def _on_build_toggle(self):
        """Enable/disable firmware path entry based on build toggle."""
        if self.build_release.get():
            self.fw_entry.configure(state="disabled")
            self.browse_btn.set_enabled(False)
        else:
            self.fw_entry.configure(state="normal")
            self.browse_btn.set_enabled(True)

    def _set_busy(self, busy: bool):
        """Switch the progress bar to/from animated indeterminate mode during build."""
        if busy:
            self.progressbar.start_indeterminate()
        else:
            self.progressbar.stop_indeterminate()
            self.progressbar.set_value(0)

    def _browse(self):
        path = filedialog.askopenfilename(
            title="Select firmware .bin",
            filetypes=[("Firmware", "*.bin"), ("All files", "*.*")],
        )
        if path:
            self.firmware_path.set(path)

    def _build_release_worker(self):
        """Build release firmware in background thread."""
        self._build_done.clear()
        self._build_result = None
        try:
            # Clean first
            subprocess.run(
                ["platformio", "run", "--target", "clean", "--environment", "esp32-c3-supermini"],
                cwd=Path(__file__).parent.parent,
                capture_output=True,
                timeout=60
            )
            # Build
            result = subprocess.run(
                ["platformio", "run", "--environment", "esp32-c3-supermini"],
                cwd=Path(__file__).parent.parent,
                capture_output=True,
                text=True,
                timeout=120
            )
            self._build_result = (result.returncode == 0, result.stderr if result.returncode != 0 else None)
        except Exception as e:
            self._build_result = (False, str(e))
        finally:
            self._build_done.set()

    def _build_release(self):
        """Build release firmware using platformio (non-blocking)."""
        self._set_busy(True)
        self.status.set("Building release...")
        self._build_done.clear()
        self._build_result = None

        thread = threading.Thread(target=self._build_release_worker, daemon=True)
        thread.start()

        # Wait with UI updates
        while not self._build_done.is_set():
            self.root.update()
            time.sleep(0.05)

        self._set_busy(False)
        success, error = self._build_result
        if success:
            firmware_bin = Path(__file__).parent.parent / ".pio" / "build" / "esp32-c3-supermini" / "firmware.bin"
            if firmware_bin.exists():
                self.firmware_path.set(str(firmware_bin))
            self.status.set("Build successful")
        else:
            self.status.set(f"Build failed: {error[:100] if error else 'Unknown error'}")

        return success

    def _scan(self):
        self.status.set("Scanning...")
        self.lst.delete(0, tk.END)
        self.devices = []
        self.scan_progress.pack(side="left", padx=(10, 0))
        self.scan_progress.start_indeterminate()
        threading.Thread(target=self._scan_worker, daemon=True).start()

    def _scan_worker(self):
        async def scan():
            devices = await BleakScanner().discover(timeout=5.0)
            return [d for d in devices if d.name]

        try:
            found = asyncio.run(scan())
        except Exception as e:
            self.status.set(f"Scan error: {e}")
            self.scan_progress.pack_forget()
            return
        self.devices = found
        self.lst.delete(0, tk.END)
        for d in found:
            self.lst.insert(tk.END, f"{d.name} [{d.address}]")
        self.status.set(f"Found {len(found)} device(s)")
        self.scan_progress.pack_forget()

    def _on_select(self, event):
        sel = self.lst.curselection()
        if not sel:
            return
        idx = sel[0]
        if idx < len(self.devices):
            self.selected_address = self.devices[idx].address

    def _start_ota(self):
        if not self.selected_address:
            messagebox.showwarning("Warning", "Select a device first")
            return

        # Build release if checkbox is checked
        if self.build_release.get():
            if not self._build_release():
                if not messagebox.askyesno("Warning", "Build failed. Continue with existing firmware?"):
                    return

        path = self.firmware_path.get().strip()
        if not path or not Path(path).exists():
            messagebox.showwarning("Warning", "Select a valid firmware .bin file")
            return
        self.status.set("OTA in progress...")
        threading.Thread(target=self._ota_worker, args=(self.selected_address, Path(path)), daemon=True).start()

    def _ota_worker(self, address: str, firmware_path: Path):
        data = firmware_path.read_bytes()
        total = len(data)
        if total == 0:
            self.status.set("Error: empty firmware")
            return

        async def run():
            self.status.set(f"Connecting to {address}...")
            try:
                async with BleakClient(address) as client:
                    if not client.is_connected:
                        self.status.set("Error: failed to connect")
                        return

                    try:
                        paired = await client.pair()
                    except Exception as e:
                        paired = True

                    self.status.set(f"Paired/bonded: {paired}")

                    self.status.set("Starting OTA...")
                    try:
                        await client.write_gatt_char(OTA_CTRL_UUID, f"start:{total}".encode("utf-8"), response=True)
                    except Exception as e:
                        self.status.set(f"Error: start command failed - {e}")
                        return

                    offset = 0
                    start = time.time()
                    while offset < total:
                        chunk = data[offset:offset + CHUNK_SIZE]
                        try:
                            await client.write_gatt_char(OTA_DATA_UUID, chunk, response=True)
                        except Exception as e:
                            self.status.set(f"Error: chunk failed at {offset} - {e}")
                            return
                        offset += len(chunk)
                        percent = offset * 100.0 / total
                        self.progress.set(percent)
                        self.status.set(f"OTA: {percent:.1f}%")

                    self.progress.set(100)

                    self.status.set("Finishing...")
                    try:
                        await client.write_gatt_char(OTA_CTRL_UUID, b"finish", response=True)
                    except Exception as e:
                        self.status.set(f"Error: finish failed - {e}")

                    elapsed = time.time() - start
                    self.status.set(f"Done in {elapsed:.1f}s ({total / elapsed:.1f} bytes/s)")
            except Exception as e:
                self.status.set(f"Error: {e}")

        asyncio.run(run())


def main():
    root = tk.Tk()
    app = BleOtaGui(root)
    root.mainloop()


if __name__ == "__main__":
    main()