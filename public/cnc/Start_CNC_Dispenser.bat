@echo off
setlocal
cd /d "%~dp0"

if not exist "%~dp0temp" mkdir "%~dp0temp"

if exist "%~dp0Mach3Bridge.exe" (
    echo [OK] Go startuvam Mach3Bridge.exe ^(port 127.0.0.1:8765^)...
    start "Mach3Bridge" /MIN "%~dp0Mach3Bridge.exe"
) else (
    echo [INFO] Mach3Bridge.exe se uste ne e kompiliran.
    echo        Za realen Mach3, startuvajte bridge\Build_Mach3Bridge.bat.
    echo        Se otvora aplikacijata ^(raboti i vo Simulator rezim^).
)

start "" "%~dp0index.html"
exit /b 0
