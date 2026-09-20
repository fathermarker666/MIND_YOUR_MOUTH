$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

if (-not (Test-Path -LiteralPath ".venv\Scripts\python.exe")) {
    throw "找不到 .venv。請先依 README.md 建立新的虛擬環境。"
}

& ".venv\Scripts\python.exe" -m PyInstaller `
    --noconfirm `
    --clean `
    --onefile `
    --windowed `
    --name INMP441_Recorder `
    --collect-submodules serial `
    --distpath "$scriptDir\dist" `
    --workpath "$scriptDir\build" `
    --specpath "$scriptDir" `
    "$scriptDir\inmp441_recorder_app.py"

if ($LASTEXITCODE -ne 0) { throw "PyInstaller 打包失敗。" }
Write-Host "完成：$scriptDir\dist\INMP441_Recorder.exe"
