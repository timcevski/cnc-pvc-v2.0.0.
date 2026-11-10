@echo off
setlocal
cd /d "%~dp0"

echo ============================================================
echo   CNC Dispenser V2.0 - Build Mach3Bridge.exe (32-bit x86)
echo ============================================================
echo.

set OUT_EXE=%~dp0..\Mach3Bridge.exe
set SRC_CPP=%~dp0Mach3Bridge.cpp

if not exist "%SRC_CPP%" (
    echo [ERROR] Mach3Bridge.cpp ne e pronajden vo %~dp0
    pause
    exit /b 1
)

:: 1. Check for MinGW-w64 32-bit cross compiler first
where i686-w64-mingw32-g++ >nul 2>nul
if %ERRORLEVEL%==0 (
    echo [OK] Pronajden i686-w64-mingw32-g++ ^(32-bit Win32^). Kompiliram...
    i686-w64-mingw32-g++ -O2 -Wall -m32 "%SRC_CPP%" -o "%OUT_EXE%" -lws2_32 -lole32 -loleaut32 -luuid -static-libgcc -static-libstdc++ -static
    goto :CHECK_RESULT
)

:: 2. Check standard g++ (MinGW / TDM-GCC on WinXP / Win7)
where g++ >nul 2>nul
if %ERRORLEVEL%==0 (
    echo [OK] Pronajden g++ ^(MinGW^). Kompiliram za 32-bit Win32 ^(-m32^)...
    g++ -O2 -Wall -m32 "%SRC_CPP%" -o "%OUT_EXE%" -lws2_32 -lole32 -loleaut32 -luuid -static-libgcc -static-libstdc++ -static
    if not %ERRORLEVEL%==0 (
        echo [INFO] Probuvam bez -static flag za postar MinGW ^(WinXP TDM-GCC^)...
        g++ -O2 -Wall -m32 "%SRC_CPP%" -o "%OUT_EXE%" -lws2_32 -lole32 -loleaut32 -luuid -static-libgcc -static-libstdc++
    )
    goto :CHECK_RESULT
)

:: 3. Check Visual Studio C++ compiler (cl.exe - run from x86 Native Tools Prompt)
where cl >nul 2>nul
if %ERRORLEVEL%==0 (
    echo [OK] Pronajden Visual C++ cl.exe. Kompiliram so /MT ^(bez DLL zavisnosti^)...
    cl /nologo /O2 /EHsc /MT "%SRC_CPP%" /Fe:"%OUT_EXE%" ws2_32.lib ole32.lib oleaut32.lib uuid.lib
    if exist "%~dp0Mach3Bridge.obj" del /q "%~dp0Mach3Bridge.obj"
    goto :CHECK_RESULT
)

echo [ERROR] Na ovoj kompjuter ne e pronajden C++ kompajler ^(g++ ili cl.exe^).
echo.
echo Kako da go kompilirate Mach3Bridge.exe ednas za sekogas:
echo   Opcija A ^(Najlesno za WinXP / Win7^):
echo     Instalirajte 32-bit MinGW / TDM-GCC ^(http://mingw-w64.org ili TDM-GCC 32-bit^)
echo     i potoa startuvajte go Build_Mach3Bridge.bat povtorno.
echo.
echo   Opcija B ^(Visual Studio C++ / Build Tools^):
echo     Otvorete "x86 Native Tools Command Prompt" i povikajte Build_Mach3Bridge.bat.
echo.
pause
exit /b 1

:CHECK_RESULT
if exist "%OUT_EXE%" (
    echo.
    echo [USPEH!] Mach3Bridge.exe e uspesno kreiran na:
    echo   %OUT_EXE%
    echo.
    echo Sega mozete da kliknete na Start_CNC_Dispenser.bat vo glavnata papka!
    echo.
    pause
    exit /b 0
) else (
    echo.
    echo [GRESKA!] Kompilacijata ne uspea. Proverete gi porakite pogore.
    pause
    exit /b 1
)
