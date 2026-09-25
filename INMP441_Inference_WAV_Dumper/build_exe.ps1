$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

$pythonExe = "C:\MIND_YOUR_MOUTH\INMP441_Recorder\.venv\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $pythonExe)) {
    throw "Existing recorder virtual environment was not found."
}

& $pythonExe -m PyInstaller --noconfirm --clean --onefile --windowed --name INMP441_Inference_WAV_Dumper --collect-submodules serial --distpath "$scriptDir\dist" --workpath "$scriptDir\build" --specpath "$scriptDir" "$scriptDir\inference_wav_dumper.py"
if ($LASTEXITCODE -ne 0) { throw "PyInstaller build failed." }
Write-Host "Complete: $scriptDir\dist\INMP441_Inference_WAV_Dumper.exe"
