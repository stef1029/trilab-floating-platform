@echo off
REM Run the Adelie host application from cmd.exe.
REM
REM   adelie                    launch the GUI
REM   adelie --config x.json    any adelie argument is passed straight through
REM   adelie setup              create .venv and install the package into it
REM
REM Adelie's --config and --recordings defaults are RELATIVE paths
REM ("config/boards.json", "recordings"), so this script changes into
REM software\ first.  Launch adelie.exe from anywhere else and it will fail to
REM find the board configuration and drop recordings wherever you happened to
REM be standing.
setlocal

set "HERE=%~dp0"
set "VENV=%HERE%.venv"
cd /d "%HERE%"

if /i "%~1"=="setup" goto setup
if /i "%~1"=="--help" goto help
if /i "%~1"=="-h" goto help

if not exist "%VENV%\Scripts\adelie.exe" (
  echo Adelie is not installed in %VENV%
  echo.
  echo Run:  %~nx0 setup
  exit /b 1
)

"%VENV%\Scripts\adelie.exe" %*
exit /b %ERRORLEVEL%


:setup
if exist "%VENV%\Scripts\python.exe" (
  echo Reusing the existing environment in %VENV%
) else (
  echo Creating %VENV%
  py -3 -m venv "%VENV%"
  if errorlevel 1 python -m venv "%VENV%"
  if errorlevel 1 (
    echo ERROR: could not create a virtual environment.  Is Python installed?
    exit /b 1
  )
)

echo Installing adelie-control and its dependencies ^(this pulls in PySide6, so
echo expect a few hundred MB on a first run^)...
"%VENV%\Scripts\python.exe" -m pip install --upgrade pip
if errorlevel 1 exit /b 1
"%VENV%\Scripts\python.exe" -m pip install -e .
if errorlevel 1 exit /b 1

echo.
echo Done.  Run "%~nx0" to start the GUI.
exit /b 0


:help
echo usage: %~nx0 [setup ^| ^<adelie arguments^>]
echo.
echo   %~nx0                  launch the Adelie GUI (last used mode)
echo   %~nx0 --simple         launch in Simple mode
echo   %~nx0 --dev            launch in Developer mode
echo   %~nx0 setup            create .venv and install the package
echo.
echo Other entry points live beside adelie.exe in .venv\Scripts\ :
echo   adelie-parse   adelie-analyse   adelie-plot   adelie-run-analysis
exit /b 0
