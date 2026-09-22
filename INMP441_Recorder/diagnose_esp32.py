"""非錄音的 ESP32 串列通訊檢測工具。"""

import time

import serial


PORT = "COM5"
BAUD_RATE = 115200


def main() -> int:
    print(f"檢查 {PORT}（{BAUD_RATE} baud）…")
    try:
        with serial.Serial(PORT, BAUD_RATE, timeout=0.25, write_timeout=3) as ser:
            # 開啟 serial port 常使 ESP32-S3 重啟，先等待它的 setup() 跑完。
            time.sleep(2.0)
            ser.reset_input_buffer()
            ser.write(b"PING\n")
            ser.flush()

            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                response = ser.readline()
                if response:
                    print("ESP32 回覆：", response.decode("ascii", errors="replace").rstrip())
                    if response.strip() == b"ERROR UNKNOWN_COMMAND":
                        print("成功：USB、COM 埠與目前韌體的雙向通訊正常。")
                        return 0
                    print("有收到資料，但不是預期回覆；請確認燒錄的是本專案韌體。")
                    return 2
    except serial.SerialTimeoutException:
        print("失敗：COM 埠可開啟，但 ESP32/USB 沒有接受寫入。")
        print("請拔插資料線、改用可傳資料的 USB 線或 USB 埠、按 EN/Reset，然後再試。")
        return 3
    except serial.SerialException as exc:
        print(f"失敗：序列埠錯誤：{exc}")
        return 4

    print("失敗：指令可送出，但 5 秒內 ESP32 沒有回覆。")
    print("請在 Arduino IDE 重新上傳 INMP441_Recorder_App_Firmware.ino，再重試。")
    return 5


if __name__ == "__main__":    raise SystemExit(main())