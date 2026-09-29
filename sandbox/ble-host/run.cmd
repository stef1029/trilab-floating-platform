@echo off
REM Run the sandbox BLE host using the Adelie virtual environment.
REM   cd sandbox\ble-host  &&  run
setlocal
set PY=%~dp0..\..\software\.venv\Scripts\python.exe
if not exist "%PY%" (
    echo ERROR: Adelie venv not found at %PY%
    echo Create it with:
    echo     cd ..\..\software
    echo     python -m venv .venv
    echo     .venv\Scripts\python.exe -m pip install -e .
    exit /b 1
)
"%PY%" "%~dp0led_button.py" %*
