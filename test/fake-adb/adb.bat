@echo off
rem Fake adb for host end-to-end tests. ASCII-only (codepage safety).
rem Command surface expected by EmmcScan: connect / -s SERIAL get-state /
rem shell getprop / push / shell chmod / shell su 0 id -u / shell su 0 BIN --json / shell rm -f
setlocal EnableDelayedExpansion
rem Arg layout when called by EmmcScan: %1=-s %2=serial %3=verb %4...=args
rem verbs: connect | get-state | shell | push
rem shell: %4=su %5=0 %6=id|...  OR  %4=getprop %5=key  OR  %4=chmod/%4=rm
if /i "%~1"=="connect" (
    echo connected to %~2
    exit /b 0
)
if /i not "%~1"=="-s" exit /b 0
set "VERB=%~3"
if /i "!VERB!"=="get-state" (
    echo device
    exit /b 0
)
if /i "!VERB!"=="push" (
    echo 1 file pushed, 0 skipped.
    exit /b 0
)
if /i not "!VERB!"=="shell" exit /b 0
set "SUB=%~4"
set "ARG=%~5"
if /i "!SUB!"=="get-state" (
    echo device
    exit /b 0
)
if /i "!SUB!"=="getprop" (
    if /i "!ARG!"=="ro.product.cpu.abi" echo arm64-v8a
    if /i "!ARG!"=="ro.product.model" echo FakeBox-Test
    exit /b 0
)
if /i "!SUB!"=="chmod" exit /b 0
if /i "!SUB!"=="rm" exit /b 0
if /i "!SUB!"=="su" (
    if /i "%~6"=="id" (
        echo 0
        exit /b 0
    )
    type "%~dp0fixture_ok.json"
    exit /b 0
)
exit /b 0
