@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   Growtopia Proxy - Build Script
echo ============================================
echo.

REM
where gcc >nul 2>&1
if errorlevel 1 (
    echo [ERROR] gcc.exe not found in PATH.
    echo         Add C:\mingw64\bin to the PATH variable, or run this
    echo         script from there.
    goto :fail
)

for /f "delims=" %%v in ('gcc -dumpversion') do set GCC_VER=%%v
echo [OK] gcc found, version: %GCC_VER%
echo.

REM
if not exist "C:\mingw64\include\openssl\ssl.h" (
    echo [ERROR] OpenSSL-headers are missing: C:\mingw64\include\openssl\ssl.h
    echo         Install the dev package from https://slproweb.com/products/Win32OpenSSL.html
    goto :fail
)
echo [OK] OpenSSL-header found.

REM 
set MISSING_LIB=0
if not exist "C:\mingw64\lib\libssl.dll.a" (
    echo [ERROR] Missing: C:\mingw64\lib\libssl.dll.a
    set MISSING_LIB=1
)
if not exist "C:\mingw64\lib\libcrypto.dll.a" (
    echo [ERROR] Missing: C:\mingw64\lib\libcrypto.dll.a
    set MISSING_LIB=1
)
if !MISSING_LIB! equ 1 (
    echo         [ERROR] OpenSSL libraries are missing.
    echo         Please install OpenSSL development package first.
    goto :fail
)
echo [OK] OpenSSL-Libraries found.

REM
set MISSING_DLL=0
if not exist "bin\libssl-3-x64.dll" (
    echo [WARNING] libssl-3-x64.dll Missing.
    set MISSING_DLL=1
)
if not exist "bin\libcrypto-3-x64.dll" (
    echo [WARNING] libcrypto-3-x64.dll Missing.
    set MISSING_DLL=1
)
if !MISSING_DLL! equ 1 (
    echo         Copying from C:\mingw64\bin...
    copy /Y "C:\mingw64\bin\libssl-3-x64.dll" bin >nul
    copy /Y "C:\mingw64\bin\libcrypto-3-x64.dll" bin >nul
    if exist "libssl-3-x64.dll" (
        echo [OK] DLLS copied.
    ) else (
        echo [ERROR] DLL copy failed.
        goto :fail
    )
) else (
    echo [OK] Runtime DLLs found.
)
echo.

REM
if not exist "enet\win32.c" (
    echo [ERROR] enet\win32.c missing
    goto :fail
)
echo [OK] ENet-sources found.
echo.

REM
echo Compiling...
echo.

if not exist bin mkdir bin

gcc -o bin\proxy.exe ^
    proxy.c hosts.c getserver.c https.c ^
    enet\callbacks.c enet\compress.c enet\host.c enet\list.c ^
    enet\packet.c enet\peer.c enet\protocol.c enet\win32.c ^
    -I. -Ienet ^
    -Wall -Wextra ^
    -lws2_32 -lwinmm -liphlpapi -lwininet ^
    C:\mingw64\lib\libssl.dll.a C:\mingw64\lib\libcrypto.dll.a

if errorlevel 1 (
    echo.
    echo ============================================
    echo   BUILD Failed - Look at the errors above
    echo ============================================
    goto :fail
)

echo.
echo ============================================
echo   BUILD SUCCEEDED: bin\proxy.exe
echo ============================================
echo.

echo Run bin\proxy.exe as an ^(administrator^),
echo.
pause
exit /b 0

:fail
echo.
pause
exit /b 1s