import json
import sys
import threading
import time
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
import tkinter as tk

from serial.tools import list_ports

from inmp441_recorder_core import RecorderError, record_wav


APP_TITLE = "INMP441 Recorder"


def app_base_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


APP_BASE_DIR = app_base_dir()
SETTINGS_FILE = APP_BASE_DIR / "inmp441_recorder_settings.json"


def load_settings() -> dict:
    if not SETTINGS_FILE.exists():
        return {}
    try:
        return json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def save_settings(settings: dict) -> None:
    try:
        SETTINGS_FILE.write_text(
            json.dumps(settings, ensure_ascii=False, indent=2), encoding="utf-8"
        )
    except OSError:
        pass


class RecorderApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.settings = load_settings()
        self.is_recording = False
        self.progress_job = None
        self.progress_start_time = 0.0
        self.progress_duration = 1.0
        self.title(APP_TITLE)
        self.geometry("520x280")
        self.minsize(480, 260)
        default_output = self.settings.get("output_dir") or str(APP_BASE_DIR / "recordings")
        self.port_var = tk.StringVar(value=self.settings.get("port", "COM5"))
        self.seconds_var = tk.IntVar(value=int(self.settings.get("seconds", 2)))
        self.filename_prefix_var = tk.StringVar(
            value=self.settings.get("filename_prefix", "record")
        )
        self.output_dir_var = tk.StringVar(value=default_output)
        self.status_var = tk.StringVar(value="就緒")
        self._build_ui()
        self.refresh_ports()

    def _build_ui(self) -> None:
        self.columnconfigure(0, weight=1)
        container = ttk.Frame(self, padding=18)
        container.grid(row=0, column=0, sticky="nsew")
        container.columnconfigure(1, weight=1)
        ttk.Label(container, text="序列埠").grid(row=0, column=0, sticky="w", pady=6)
        port_row = ttk.Frame(container)
        port_row.grid(row=0, column=1, sticky="ew", pady=6)
        port_row.columnconfigure(0, weight=1)
        self.port_combo = ttk.Combobox(port_row, textvariable=self.port_var, state="readonly")
        self.port_combo.grid(row=0, column=0, sticky="ew")
        ttk.Button(port_row, text="重新整理", command=self.refresh_ports).grid(
            row=0, column=1, padx=(8, 0)
        )
        ttk.Label(container, text="錄音秒數").grid(row=1, column=0, sticky="w", pady=6)
        ttk.Spinbox(container, from_=1, to=10, textvariable=self.seconds_var, width=8).grid(
            row=1, column=1, sticky="w", pady=6
        )
        ttk.Label(container, text="檔名前綴").grid(row=2, column=0, sticky="w", pady=6)
        ttk.Entry(container, textvariable=self.filename_prefix_var).grid(
            row=2, column=1, sticky="ew", pady=6
        )
        ttk.Label(container, text="儲存資料夾").grid(row=3, column=0, sticky="w", pady=6)
        folder_row = ttk.Frame(container)
        folder_row.grid(row=3, column=1, sticky="ew", pady=6)
        folder_row.columnconfigure(0, weight=1)
        ttk.Entry(folder_row, textvariable=self.output_dir_var).grid(row=0, column=0, sticky="ew")
        ttk.Button(folder_row, text="選擇", command=self.choose_output_dir).grid(
            row=0, column=1, padx=(8, 0)
        )
        self.record_button = ttk.Button(container, text="開始錄音", command=self.start_recording)
        self.record_button.grid(row=4, column=0, columnspan=2, sticky="ew", pady=(18, 8))
        self.progress = ttk.Progressbar(container, mode="determinate", maximum=100)
        self.progress.grid(row=5, column=0, columnspan=2, sticky="ew", pady=6)
        ttk.Label(container, textvariable=self.status_var).grid(
            row=6, column=0, columnspan=2, sticky="w", pady=(10, 0)
        )

    def refresh_ports(self) -> None:
        ports = [port.device for port in list_ports.comports()]
        self.port_combo["values"] = ports
        current = self.port_var.get()
        if current in ports:
            self.port_var.set(current)
        elif ports:
            self.port_var.set(ports[0])
        self.status_var.set("就緒" if ports else "找不到序列埠，請確認 ESP32-S3 已連接")

    def choose_output_dir(self) -> None:
        selected = filedialog.askdirectory(
            title="選擇 WAV 儲存資料夾", initialdir=self.output_dir_var.get() or str(Path.cwd())
        )
        if selected:
            self.output_dir_var.set(selected)
            self._persist_settings()

    def start_recording(self) -> None:
        if self.is_recording:
            return
        port = self.port_var.get().strip()
        filename_prefix = self.filename_prefix_var.get().strip()
        output_dir = self.output_dir_var.get().strip()
        if not port:
            messagebox.showerror(APP_TITLE, "請先選擇序列埠。")
            return
        if not output_dir:
            messagebox.showerror(APP_TITLE, "請先選擇儲存資料夾。")
            return
        try:
            seconds = int(self.seconds_var.get())
        except (TypeError, ValueError):
            messagebox.showerror(APP_TITLE, "錄音秒數必須是數字。")
            return
        self._persist_settings()
        self._set_recording_state(True, "準備錄音...")
        threading.Thread(
            target=self._record_worker, args=(port, seconds, output_dir, filename_prefix), daemon=True
        ).start()

    def _record_worker(self, port: str, seconds: int, output_dir: str, filename_prefix: str) -> None:
        try:
            result = record_wav(
                port, seconds, output_dir, filename_prefix, connect_delay_seconds=0.1,
                on_recording_started=lambda: self.after(0, self._record_started, seconds),
            )
        except RecorderError as error:
            self.after(0, self._record_failed, str(error))
        except Exception as error:
            self.after(0, self._record_failed, f"未知錯誤：{error}")
        else:
            self.after(0, self._record_done, result.path)

    def _record_started(self, seconds: int) -> None:
        self._set_recording_state(True, f"錄音中：{seconds} 秒", seconds)

    def _record_done(self, path: Path) -> None:
        self._set_recording_state(False, f"完成：{path}")
        # 成功時只更新狀態列；失敗才以對話框要求使用者確認。

    def _record_failed(self, message: str) -> None:
        self._set_recording_state(False, f"錯誤：{message}")
        messagebox.showerror(APP_TITLE, message)

    def _set_recording_state(self, recording: bool, status: str, seconds: int | None = None) -> None:
        self.is_recording = recording
        self.status_var.set(status)
        self.record_button.configure(state="disabled" if recording else "normal")
        if recording:
            if seconds is None:
                if self.progress_job is not None:
                    self.after_cancel(self.progress_job)
                    self.progress_job = None
                self.progress["value"] = 0
            else:
                self.progress["value"] = 0
                self.progress_duration = float(seconds)
                self.progress_start_time = time.monotonic()
                self._update_progress_by_time()
        else:
            if self.progress_job is not None:
                self.after_cancel(self.progress_job)
                self.progress_job = None
            self.progress["value"] = 100

    def _update_progress_by_time(self) -> None:
        elapsed = time.monotonic() - self.progress_start_time
        percent = min(100.0, (elapsed / self.progress_duration) * 100.0)
        self.progress["value"] = percent
        if self.is_recording and percent < 100.0:
            self.progress_job = self.after(50, self._update_progress_by_time)
        else:
            self.progress_job = None

    def _persist_settings(self) -> None:
        save_settings({
            "port": self.port_var.get(),
            "seconds": self.seconds_var.get(),
            "filename_prefix": self.filename_prefix_var.get(),
            "output_dir": self.output_dir_var.get(),
        })


if __name__ == "__main__":
    RecorderApp().mainloop()
