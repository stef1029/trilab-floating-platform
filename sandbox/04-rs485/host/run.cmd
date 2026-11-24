@echo off
setlocal
set PY=%~dp0..\..\..\software\.venv\Scripts\python.exe
set PYTHONPATH=%~dp0..\..\..\software\src
if not exist "%PY%" (echo ERROR: Adelie venv not found & exit /b 1)
"%PY%" "%~dp0node_led.py" %*
