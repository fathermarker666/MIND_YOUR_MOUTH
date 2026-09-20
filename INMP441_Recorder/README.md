# INMP441 Recorder：可自行維護的版本

## 原始碼位置

- `INMP441_Recorder_App_Firmware.ino`：ESP32 韌體；麥克風增益在第 13 行的 `MIC_GAIN`。
- `inmp441_recorder_app.py`：電腦上的 Tkinter 視窗程式。
- `inmp441_recorder_core.py`：序列埠通訊、WAV 驗證與輸出。
- `build_exe.ps1`：把 Python 視窗程式打包為 EXE。

EXE **不會**改變 INMP441 的硬體增益。每次調整 `MIC_GAIN`，要先用 Arduino IDE 或 PlatformIO 重新燒錄 `.ino` 到 ESP32；只有修改視窗、存檔方式或電腦端處理時，才需要重新打包 EXE。

## 建議先搬到純英文路徑

目前專案在 `C:\電子科專題\...`，現有 `.venv` 已經因含中文路徑而無法啟動。請先把這個整個資料夾複製到例如 `C:\Projects\INMP441_Recorder`，不要搬動原本資料夾，以保留備份。

接著在 VS Code 選 **File > Open Workspace from File**，選 `INMP441_Recorder.code-workspace`。不要再開 `__pycache__.code-workspace`：那個檔案指向舊的暫存建置目錄，所以不是這個專案的正確工作區。

## 第一次設定 Python

在 VS Code 的 Terminal（PowerShell）執行：

```powershell
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

然後按 `Ctrl+Shift+P`，選 **Python: Select Interpreter**，選擇 `.venv\Scripts\python.exe`。若看不到 Python 顏色、執行按鈕或這個命令，請從 Extensions 安裝發行者為 Microsoft 的 **Python** 擴充套件。

開發時先直接執行，不必每次打包：

```powershell
.\.venv\Scripts\python.exe .\inmp441_recorder_app.py
```

## 測試增益

1. 用 Arduino IDE 開啟 `INMP441_Recorder_App_Firmware.ino`。
2. 將 `MIC_GAIN` 依序測試，例如 `2`、`4`、`6`、`8`；每改一次按 Upload 燒錄 ESP32。
3. 直接執行 Python 程式或目前 EXE 錄相同內容，聽 WAV 是否太小、爆音或削波。超過 32767/-32768 的聲音會被截平，聽起來會破。
4. 選沒有明顯削波、但人聲足夠大的最低增益。

## 打包 EXE

確認視窗版 Python 程式可正常錄音後，在專案根目錄執行：

```powershell
.\build_exe.ps1
```

成品會在 `dist\INMP441_Recorder.exe`。原來的根目錄 EXE 不會被覆蓋。
