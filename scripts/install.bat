@echo off
REM InstantDB Installation Script for Windows (Batch)
REM Usage: curl -sSf https://install.instantdb.com/windows.bat | cmd

setlocal EnableDelayedExpansion

REM Configuration
set "GITHUB_REPO=your-org/instantdb"
set "BINARY_NAME=instantdb.exe"
set "INSTALL_DIR=%ProgramFiles%\InstantDB"
set "TMP_DIR=%TEMP%\instantdb-install"

REM Colors (basic support)
set "COLOR_INFO=echo [94m[INFO][0m"
set "COLOR_WARN=echo [93m[WARN][0m"
set "COLOR_ERROR=echo [91m[ERROR][0m"
set "COLOR_SUCCESS=echo [92m[SUCCESS][0m"

echo Starting InstantDB installation...

REM Check if curl is available
where curl >nul 2>nul
if %errorlevel% neq 0 (
    %COLOR_ERROR% curl is required but not found. Please install curl and try again.
    exit /b 1
)

REM Check if tar is available (Windows 10 build 17063+ has built-in tar)
where tar >nul 2>nul
if %errorlevel% neq 0 (
    %COLOR_WARN% tar not found. Checking for PowerShell...
    where powershell >nul 2>nul
    if !errorlevel! neq 0 (
        %COLOR_ERROR% Neither tar nor PowerShell found. Cannot proceed.
        exit /b 1
    )
    %COLOR_INFO% Using PowerShell for extraction
    set "USE_POWERSHELL=1"
)

REM Detect architecture
set "ARCH=x64"
if "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "ARCH=arm64"

%COLOR_INFO% Detected architecture: %ARCH%

REM Create temporary directory
if exist "%TMP_DIR%" rmdir /s /q "%TMP_DIR%"
mkdir "%TMP_DIR%"

REM Download latest release
%COLOR_INFO% Downloading InstantDB for Windows %ARCH%...
set "DOWNLOAD_URL=https://github.com/%GITHUB_REPO%/releases/latest/download/instantdb-windows-%ARCH%.zip"
set "ZIP_FILE=%TMP_DIR%\instantdb.zip"

curl -sSfL "%DOWNLOAD_URL%" -o "%ZIP_FILE%"
if %errorlevel% neq 0 (
    %COLOR_ERROR% Failed to download InstantDB
    goto cleanup
)

%COLOR_SUCCESS% Download completed

REM Extract archive
%COLOR_INFO% Extracting archive...
if defined USE_POWERSHELL (
    powershell -Command "Expand-Archive -Path '%ZIP_FILE%' -DestinationPath '%TMP_DIR%' -Force"
) else (
    tar -xf "%ZIP_FILE%" -C "%TMP_DIR%"
)

if %errorlevel% neq 0 (
    %COLOR_ERROR% Failed to extract archive
    goto cleanup
)

REM Create install directory
%COLOR_INFO% Installing InstantDB...
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"

REM Copy binary
copy "%TMP_DIR%\%BINARY_NAME%" "%INSTALL_DIR%\%BINARY_NAME%" >nul
if %errorlevel% neq 0 (
    %COLOR_ERROR% Failed to install binary. You may need to run as Administrator.
    goto cleanup
)

%COLOR_SUCCESS% InstantDB installed to %INSTALL_DIR%\%BINARY_NAME%

REM Add to PATH
%COLOR_INFO% Adding InstantDB to PATH...
echo %PATH% | findstr /C:"%INSTALL_DIR%" >nul
if %errorlevel% equ 0 (
    %COLOR_INFO% InstantDB is already in PATH
) else (
    REM Add to system PATH (requires admin privileges)
    reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v Path /t REG_EXPAND_SZ /d "%PATH%;%INSTALL_DIR%" /f >nul 2>nul
    if !errorlevel! equ 0 (
        %COLOR_SUCCESS% Added InstantDB to system PATH
        set "PATH=%PATH%;%INSTALL_DIR%"
    ) else (
        %COLOR_WARN% Could not add to system PATH (requires administrator privileges)
        %COLOR_INFO% Please add manually: %INSTALL_DIR%
        %COLOR_INFO% Or run this script as Administrator
    )
)

REM Verify installation
%COLOR_INFO% Verifying installation...
if exist "%INSTALL_DIR%\%BINARY_NAME%" (
    "%INSTALL_DIR%\%BINARY_NAME%" --version >nul 2>nul
    if !errorlevel! equ 0 (
        %COLOR_SUCCESS% InstantDB installed successfully!
        %COLOR_SUCCESS% Location: %INSTALL_DIR%\%BINARY_NAME%
    ) else (
        %COLOR_WARN% Binary installed but version check failed
    )
) else (
    %COLOR_ERROR% Installation verification failed
    goto cleanup
)

%COLOR_SUCCESS% 🚀 InstantDB installation completed!
echo.
echo Next steps:
echo   1. Restart your command prompt or open a new one
echo   2. Start the InstantDB server: instantdb server
echo   3. Check out the documentation: https://docs.instantdb.com
echo   4. Try the C# quickstart: https://docs.instantdb.com/csharp
echo.

goto end

:cleanup
%COLOR_INFO% Cleaning up...
if exist "%TMP_DIR%" rmdir /s /q "%TMP_DIR%"
exit /b 1

:end
%COLOR_INFO% Cleaning up...
if exist "%TMP_DIR%" rmdir /s /q "%TMP_DIR%"
exit /b 0