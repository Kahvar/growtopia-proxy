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

REM Determine OPENSSL_DIR. If user set OPENSSL_DIR honor it; otherwise probe common locations.
if "%OPENSSL_DIR%"=="" (
    if exist "C:\mingw64\include\openssl\ssl.h" (
        set "OPENSSL_DIR=C:\mingw64"
    ) else if exist "C:\msys64\ucrt64\include\openssl\ssl.h" (
        set "OPENSSL_DIR=C:\msys64\ucrt64"
    ) else if exist "C:\msys64\mingw64\include\openssl\ssl.h" (
        set "OPENSSL_DIR=C:\msys64\mingw64"
    ) else if exist "C:\OpenSSL-Win64\include\openssl\ssl.h" (
        set "OPENSSL_DIR=C:\OpenSSL-Win64"
    ) else if exist "C:\Program Files\OpenSSL-Win64\include\openssl\ssl.h" (
        set "OPENSSL_DIR=C:\Program Files\OpenSSL-Win64"
    ) else (
        set "OPENSSL_DIR=C:\mingw64"
    )
)
echo [INFO] Using OPENSSL_DIR=%OPENSSL_DIR%

if not exist "%OPENSSL_DIR%\include\openssl\ssl.h" (
    echo [ERROR] OpenSSL-headers are missing: %OPENSSL_DIR%\include\openssl\ssl.h
    echo         Install the dev package or set OPENSSL_DIR to your OpenSSL installation path.
    goto :fail
)
echo [OK] OpenSSL-header found.

REM 
set OPENSSL_LIB_SSL=
set OPENSSL_LIB_CRYPTO=
set MISSING_LIB=0
rem Prefer libssl.dll.a/libcrypto.dll.a, but accept libssl.a/libcrypto.a (MSYS/MinGW differences)
if exist "%OPENSSL_DIR%\lib\libssl.dll.a" (
    set "OPENSSL_LIB_SSL=%OPENSSL_DIR%\lib\libssl.dll.a"
) else if exist "%OPENSSL_DIR%\lib\libssl.a" (
    set "OPENSSL_LIB_SSL=%OPENSSL_DIR%\lib\libssl.a"
) else (
    echo [ERROR] Missing OpenSSL lib: libssl.dll.a or libssl.a under %OPENSSL_DIR%\lib
    set MISSING_LIB=1
)

if exist "%OPENSSL_DIR%\lib\libcrypto.dll.a" (
    set "OPENSSL_LIB_CRYPTO=%OPENSSL_DIR%\lib\libcrypto.dll.a"
) else if exist "%OPENSSL_DIR%\lib\libcrypto.a" (
    set "OPENSSL_LIB_CRYPTO=%OPENSSL_DIR%\lib\libcrypto.a"
) else (
    echo [ERROR] Missing OpenSSL lib: libcrypto.dll.a or libcrypto.a under %OPENSSL_DIR%\lib
    set MISSING_LIB=1
)

if !MISSING_LIB! equ 1 (
    echo         [ERROR] OpenSSL libraries are missing.
    echo         Please install OpenSSL development package first or set OPENSSL_DIR to the correct path.
    goto :fail
)
echo [OK] OpenSSL-Libraries found: %OPENSSL_LIB_SSL% %OPENSSL_LIB_CRYPTO%

REM Ensure bin directory exists before copying runtime DLLs
if not exist bin mkdir bin

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
    echo         Copying from %OPENSSL_DIR%\bin...
    copy /Y "%OPENSSL_DIR%\bin\libssl-3-x64.dll" bin >nul
    copy /Y "%OPENSSL_DIR%\bin\libcrypto-3-x64.dll" bin >nul
    if exist "bin\libssl-3-x64.dll" (
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
    -lws2_32 -lwinmm -liphlpapi -lwininet -ldnsapi ^
    %OPENSSL_LIB_SSL% %OPENSSL_LIB_CRYPTO%

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