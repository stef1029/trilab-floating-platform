@echo off
REM Build, flash or recover the Nordic (Zephyr) firmware from cmd.exe.
REM
REM   nordic hub                 build for the BMD-340 module / hub PCB
REM   nordic hub dk              build for the nRF52840 DK breadboard rig
REM   nordic ttl_generator              build for the nRF54L15 DK
REM
REM   nordic hub flash           flash the hub PCB through the DK's Debug Out
REM   nordic hub recover         unlock (mass erase!) and flash the hub PCB
REM   nordic hub dk flash        flash the DK itself over USB
REM   nordic ttl_generator flash
REM
REM The hub PCB has no USB debugger of its own.  It is reached over SWD from
REM the nRF52840 DK's "Debug out" header, which retargets the DK's on-board
REM J-Link to the external chip as soon as it senses VTref.  DK_SERIAL picks
REM which probe to use when several are plugged in.
REM
REM Everything runs inside "nrfutil sdk-manager toolchain launch", which is
REM what puts west, cmake, ninja, arm-none-eabi-gcc and pyocd on PATH.
setlocal

set "NCS_VERSION=v3.4.1"
set "NRFUTIL=C:\ncs\tools\nrfutil.exe"
set "ZEPHYR_BASE=C:/ncs/%NCS_VERSION%/zephyr"
set "HERE=%~dp0"
if not defined DK_SERIAL set "DK_SERIAL=1050230658"

set "APP=%~1"
set "ARG2=%~2"
set "ARG3=%~3"

if "%APP%"=="" goto usage

REM ---- second word is either a variant or an action ----
set "VARIANT=%ARG2%"
set "ACTION=%ARG3%"
if /i "%ARG2%"=="flash"   ( set "VARIANT=" & set "ACTION=flash" )
if /i "%ARG2%"=="recover" ( set "VARIANT=" & set "ACTION=recover" )
if "%ACTION%"=="" set "ACTION=build"

REM ---- board, build directory, and how the probe reaches the chip ----
if /i "%APP%"=="hub" (
  if /i "%VARIANT%"=="dk" (
    set "BOARD=nrf52840dk/nrf52840"
    set "BUILD=build-dk"
    set "PROBE="
  ) else (
    set "BOARD=ubx_bmd340eval/nrf52840"
    set "BUILD=build-bmd340eval"
    set "PROBE=--dev-id %DK_SERIAL%"
  )
) else if /i "%APP%"=="ttl_generator" (
  set "BOARD=nrf54l15dk/nrf54l15/cpuapp"
  set "BUILD=build-dk"
  set "PROBE="
) else (
  goto usage
)

cd /d "%HERE%"

if /i "%ACTION%"=="recover" (
  echo.
  echo *** This MASS ERASES the target before flashing. ***
  echo.
  echo Recovering and flashing %APP% from %APP%\%BUILD%
  "%NRFUTIL%" sdk-manager toolchain launch --ncs-version %NCS_VERSION% -- west flash -r nrfutil --recover %PROBE% --build-dir "%APP%\%BUILD%"
  exit /b %ERRORLEVEL%
)

if /i "%ACTION%"=="flash" (
  echo Flashing %APP% from %APP%\%BUILD%
  "%NRFUTIL%" sdk-manager toolchain launch --ncs-version %NCS_VERSION% -- west flash -r nrfutil %PROBE% --build-dir "%APP%\%BUILD%"
  exit /b %ERRORLEVEL%
)

echo Building %APP% for %BOARD%  -^>  %APP%\%BUILD%   (NCS %NCS_VERSION%)
"%NRFUTIL%" sdk-manager toolchain launch --ncs-version %NCS_VERSION% -- west build -p always -b "%BOARD%" "%APP%" --build-dir "%APP%\%BUILD%"
exit /b %ERRORLEVEL%

:usage
echo usage: nordic {hub ^[dk^] ^| ttl_generator} ^[flash ^| recover^] 1>&2
exit /b 2
