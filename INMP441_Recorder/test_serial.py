import re
import struct
import time
from pathlib import Path

import serial

PORT = "COM5"
SECONDS = 2
SAMPLE_RATE = 16000
PCM_BYTES = SAMPLE_RATE * SECONDS * 2
EXPECTED_WAV_BYTES = 44 + PCM_BYTES


def read_exact(ser, size, timeout_seconds=15):
    received = bytearray()
    deadline = time.monotonic() + timeout_seconds

    while len(received) < size:
        if time.monotonic() > deadline:
            raise TimeoutError(
                f"接收逾時：{len(received)} / {size} bytes"
            )

        chunk = ser.read(min(4096, size - len(received)))
        if chunk:
            received.extend(chunk)
            print(f"\r接收中：{len(received)} / {size} bytes", end="", flush=True)

    print()
    return bytes(received)


with serial.Serial(PORT, 115200, timeout=0.25) as ser:
    time.sleep(2)
    ser.reset_input_buffer()

    ser.write(f"REC {SECONDS}\n".encode("ascii"))
    ser.flush()
    print(f"已要求錄音 {SECONDS} 秒，等待 ESP32…")

    first_line = ser.readline()
    print("ESP32：", repr(first_line))

    match = re.fullmatch(rb"WAV_BEGIN (\d+)\r?\n", first_line)
    if not match:
        raise RuntimeError("韌體回應不正確；請確認已燒錄新版韌體。")

    expected_from_board = int(match.group(1))
    if expected_from_board != EXPECTED_WAV_BYTES:
        raise RuntimeError(
            f"長度不正確：板子說 {expected_from_board}，預期 {EXPECTED_WAV_BYTES}"
        )

    wav = read_exact(ser, expected_from_board)

    if wav[0:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise RuntimeError("收到的資料不是 WAV。")

    if struct.unpack_from("<I", wav, 40)[0] != PCM_BYTES:
        raise RuntimeError("WAV 音訊長度欄位不正確。")

    end_line = ser.readline()
    if end_line.strip() != b"WAV_END":
        raise RuntimeError(f"沒有收到正確的 WAV_END：{end_line!r}")

output = Path(__file__).resolve().parent / "firmware_test_2_seconds.wav"
output.write_bytes(wav)
print(f"成功：{output}")