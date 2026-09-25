"""Save the latest one-second window from self_test_demo01_diagnostics."""

from __future__ import annotations

import re
import struct
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import serial
from serial.tools import list_ports

BAUD_RATE = 115200
SAMPLE_RATE = 16000
SECONDS = 1
EXPECTED_WAV_BYTES = 44 + SAMPLE_RATE * SECONDS * 2
DEFAULT_RECORDINGS_FOLDER = Path(r"C:\MIND_YOUR_MOUTH\INMP441_Inference_WAV_Dumper\RECODEING")


def next_path(folder: Path, prefix: str) -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    safe = "".join("_" if char in '<>:"/\\|?*' else char for char in prefix).strip(" .") or "inference_window"
    for index in range(1, 10000):
        candidate = folder / f"{safe}_{index:04d}.wav"
        if not candidate.exists():
            return candidate
    raise RuntimeError("輸出資料夾中的檔案太多。")


def read_exact(port: serial.Serial, size: int, timeout_seconds: float = 15.0) -> bytes:
    received = bytearray()
    deadline = time.monotonic() + timeout_seconds
    while len(received) < size:
        if time.monotonic() > deadline:
            raise TimeoutError(f"WAV 接收逾時：{len(received)} / {size} bytes")
        chunk = port.read(min(4096, size - len(received)))
        if chunk:
            received.extend(chunk)
    return bytes(received)


def request_next_window(port: serial.Serial) -> tuple[bytes, str]:
    """Ask diagnostics to capture the future second beginning now."""
    port.reset_input_buffer()
    port.write(b"REC 1\n")
    port.flush()

    deadline = time.monotonic() + 4.0
    match = None
    while time.monotonic() < deadline:
        candidate = re.fullmatch(rb"WAV_BEGIN (\d+)\r?\n", port.readline())
        if candidate:
            match = candidate
            break
    if match is None:
        raise RuntimeError("沒有收到 WAV_BEGIN。確認新版 diagnostics 已燒錄，且 Serial Monitor 已關閉。")

    wav_size = int(match.group(1))
    if wav_size != EXPECTED_WAV_BYTES:
        raise RuntimeError(f"WAV 長度不符：板子回報 {wav_size}，預期 {EXPECTED_WAV_BYTES}。")
    wav = read_exact(port, wav_size)
    if wav[0:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise RuntimeError("收到的資料不是有效 WAV。")
    if struct.unpack_from("<I", wav, 40)[0] != SAMPLE_RATE * SECONDS * 2:
        raise RuntimeError("WAV 音訊長度欄位不正確。")
    if port.readline().strip() != b"WAV_END":
        raise RuntimeError("沒有收到 WAV_END。")

    # diagnostics prints this immediately after WAV_END. Read it here because
    # this application owns the COM port during the transfer.
    scores: list[tuple[str, str]] = []
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline and len(scores) < 3:
        line = port.readline().decode("utf-8", errors="replace").strip()
        found = re.fullmatch(r"([A-Za-z0-9_]+)\s+([0-9]+(?:\.[0-9]+)?)", line)
        if found:
            scores.append((found.group(1), found.group(2)))
    if len(scores) < 3:
        raise RuntimeError("WAV 已收到，但沒有收到板端同一 WAV 的三類分類結果。請確認已燒錄最新版 diagnostics。")
    return wav, ", ".join(f"{label}={value}" for label, value in scores)


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Inference WAV Dumper")
        self.resizable(False, False)
        self.port = tk.StringVar()
        # __file__ points at PyInstaller's temporary _MEI folder in the EXE;
        # recordings must therefore live in a stable user folder instead.
        self.folder = tk.StringVar(value=str(DEFAULT_RECORDINGS_FOLDER))
        self.prefix = tk.StringVar(value="inference_window")
        self.status = tk.StringVar(value="正在準備序列埠…")
        self.serial_port: serial.Serial | None = None
        self.serial_lock = threading.Lock()
        frame = ttk.Frame(self, padding=16)
        frame.grid()
        ttk.Label(frame, text="序列埠").grid(row=0, column=0, sticky="w", pady=4)
        self.port_box = ttk.Combobox(frame, width=40, textvariable=self.port, state="readonly")
        self.port_box.grid(row=0, column=1, padx=(12, 8), pady=4)
        ttk.Button(frame, text="重新整理", command=self.refresh_ports).grid(row=0, column=2, pady=4)
        ttk.Label(frame, text="檔名前綴").grid(row=1, column=0, sticky="w", pady=4)
        ttk.Entry(frame, width=48, textvariable=self.prefix).grid(row=1, column=1, columnspan=2, sticky="we", pady=4)
        ttk.Label(frame, text="儲存資料夾").grid(row=2, column=0, sticky="w", pady=4)
        ttk.Entry(frame, width=40, textvariable=self.folder).grid(row=2, column=1, padx=(12, 8), pady=4)
        ttk.Button(frame, text="選擇", command=self.choose_folder).grid(row=2, column=2, pady=4)
        self.export_button = ttk.Button(frame, text="立即錄下一秒推論 WAV", command=self.start_export, state="disabled")
        self.export_button.grid(row=3, column=0, columnspan=3, sticky="we", pady=(14, 8))
        ttk.Label(frame, textvariable=self.status, wraplength=440).grid(row=4, column=0, columnspan=3, sticky="w")
        self.refresh_ports()
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.after(100, self.prepare_connection)

    def refresh_ports(self) -> None:
        ports = [item.device for item in list_ports.comports()]
        self.port_box["values"] = ports
        if self.port.get() not in ports:
            self.port.set(ports[0] if ports else "")

    def prepare_connection(self) -> None:
        if not self.port.get():
            self.status.set("找不到序列埠；插入 ESP32 後按重新整理。")
            return
        self.export_button.configure(state="disabled")
        self.status.set("正在連線，請保持 Arduino Serial Monitor 關閉…")
        threading.Thread(target=self.connect, daemon=True).start()

    def connect(self) -> None:
        try:
            with self.serial_lock:
                if self.serial_port is not None:
                    self.serial_port.close()
                self.serial_port = serial.Serial(self.port.get(), BAUD_RATE, timeout=0.25, write_timeout=3)
                time.sleep(2.0)  # ESP32 may reset when this app first opens its USB serial port.
                self.serial_port.reset_input_buffer()
        except Exception as exc:
            self.after(0, lambda message=str(exc): self.finish_error(f"無法連線：{message}"))
        else:
            self.after(0, self.finish_ready)

    def finish_ready(self) -> None:
        self.status.set("已準備好：按下後立刻說目標詞，程式會收完整下一秒。")
        self.export_button.configure(state="normal")

    def choose_folder(self) -> None:
        chosen = filedialog.askdirectory(initialdir=self.folder.get() or None)
        if chosen:
            self.folder.set(chosen)

    def start_export(self) -> None:
        if not self.port.get():
            messagebox.showerror("沒有序列埠", "請選擇 ESP32 的 COM 埠。")
            return
        self.export_button.configure(state="disabled")
        self.status.set("錄音中：現在直接說目標詞…")
        threading.Thread(target=self.export, daemon=True).start()

    def export(self) -> None:
        try:
            with self.serial_lock:
                if self.serial_port is None or not self.serial_port.is_open:
                    raise RuntimeError("序列埠尚未準備好。")
                wav, on_device_scores = request_next_window(self.serial_port)
            output = next_path(Path(self.folder.get()), self.prefix.get())
            output.write_bytes(wav)
        except Exception as exc:
            self.after(0, lambda message=str(exc): self.finish_error(message))
        else:
            self.after(0, lambda: self.finish_success(output, on_device_scores))

    def finish_success(self, output: Path, on_device_scores: str) -> None:
        self.status.set(f"完成：{output}\n板端同一 WAV：{on_device_scores}")
        self.export_button.configure(state="normal")

    def finish_error(self, message: str) -> None:
        self.status.set(f"錯誤：{message}")
        self.export_button.configure(state="normal")

    def close(self) -> None:
        with self.serial_lock:
            if self.serial_port is not None:
                self.serial_port.close()
                self.serial_port = None
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
