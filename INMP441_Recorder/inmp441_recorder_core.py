import re
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import serial


BAUD_RATE = 115200
SAMPLE_RATE = 16000
SAMPLE_WIDTH_BYTES = 2
WAV_HEADER_BYTES = 44
MIN_SECONDS = 1
MAX_SECONDS = 10


class RecorderError(Exception):
    """Base error for user-facing recorder failures."""


class FirmwareError(RecorderError):
    """The board replied with an unexpected protocol response."""


class RecordingTimeout(RecorderError):
    """The board did not send the expected data in time."""


@dataclass(frozen=True)
class RecordingResult:
    path: Path
    seconds: int
    bytes_written: int


def expected_wav_size(seconds: int) -> int:
    pcm_bytes = SAMPLE_RATE * seconds * SAMPLE_WIDTH_BYTES
    return WAV_HEADER_BYTES + pcm_bytes


def validate_seconds(seconds: int) -> int:
    try:
        value = int(seconds)
    except (TypeError, ValueError) as exc:
        raise RecorderError("錄音秒數必須是數字。") from exc

    if value < MIN_SECONDS or value > MAX_SECONDS:
        raise RecorderError(f"錄音秒數目前限制為 {MIN_SECONDS} 到 {MAX_SECONDS} 秒。")
    return value


def sanitize_filename_prefix(prefix: str) -> str:
    value = str(prefix or "").strip()
    for char in '<>:"/\\|?*':
        value = value.replace(char, "_")
    return value.strip(" .") or "record"


def next_recording_path(output_dir: Path, prefix: str = "record") -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    safe_prefix = sanitize_filename_prefix(prefix)
    for index in range(1, 10000):
        candidate = output_dir / f"{safe_prefix}_{index:04d}.wav"
        if not candidate.exists():
            return candidate
    raise RecorderError("錄音檔太多，請換一個資料夾或清理舊檔。")


def _read_line(ser: serial.Serial, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        line = ser.readline()
        if line:
            return line
    raise RecordingTimeout("等待 ESP32 回應逾時。")


def _read_exact(ser: serial.Serial, size: int, timeout_seconds: float) -> bytes:
    received = bytearray()
    deadline = time.monotonic() + timeout_seconds
    while len(received) < size:
        if time.monotonic() > deadline:
            raise RecordingTimeout(f"接收 WAV 逾時：{len(received)} / {size} bytes。")
        chunk = ser.read(min(4096, size - len(received)))
        if chunk:
            received.extend(chunk)
    return bytes(received)


def _validate_wav(wav: bytes, seconds: int) -> None:
    if len(wav) != expected_wav_size(seconds):
        raise FirmwareError(f"WAV 長度不正確：收到 {len(wav)} bytes。")
    if wav[0:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise FirmwareError("收到的資料不是有效 WAV。")
    pcm_bytes = SAMPLE_RATE * seconds * SAMPLE_WIDTH_BYTES
    header_pcm_bytes = struct.unpack_from("<I", wav, 40)[0]
    if header_pcm_bytes != pcm_bytes:
        raise FirmwareError(
            f"WAV 音訊長度欄位不正確：收到 {header_pcm_bytes}，預期 {pcm_bytes}。"
        )


def record_wav(
    port: str,
    seconds: int,
    output_dir: str | Path,
    filename_prefix: str = "record",
    *,
    connect_delay_seconds: float = 2.0,
    marker_timeout_seconds: float = 5.0,
    read_timeout_seconds: float | None = None,
    on_recording_started: Callable[[], None] | None = None,
) -> RecordingResult:
    seconds = validate_seconds(seconds)
    output_dir = Path(output_dir)
    output_path = next_recording_path(output_dir, filename_prefix)
    expected_size = expected_wav_size(seconds)
    read_timeout = read_timeout_seconds or max(10.0, seconds + 8.0)

    try:
        with serial.Serial(port, BAUD_RATE, timeout=0.25, write_timeout=3) as ser:
            if connect_delay_seconds > 0:
                time.sleep(connect_delay_seconds)
            ser.reset_input_buffer()
            ser.write(f"REC {seconds}\n".encode("ascii"))
            ser.flush()
            if on_recording_started is not None:
                on_recording_started()
            marker = _read_line(ser, marker_timeout_seconds)
            match = re.fullmatch(rb"WAV_BEGIN (\d+)\r?\n", marker)
            if not match:
                raise FirmwareError(
                    f"韌體回應不正確：{marker!r}。請確認已燒錄新版韌體。"
                )
            board_size = int(match.group(1))
            if board_size != expected_size:
                raise FirmwareError(
                    f"WAV 長度不一致：板子說 {board_size}，程式預期 {expected_size}。"
                )
            wav = _read_exact(ser, board_size, read_timeout)
            _validate_wav(wav, seconds)
            end_line = _read_line(ser, 3.0)
            if end_line.strip() != b"WAV_END":
                raise FirmwareError(f"沒有收到正確的 WAV_END：{end_line!r}。")
    except serial.SerialException as exc:
        raise RecorderError(f"序列埠錯誤：{exc}") from exc

    output_path.write_bytes(wav)
    return RecordingResult(output_path, seconds, len(wav))
