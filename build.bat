@echo off
setlocal

for %%I in ("%~dp0.") do set "ROOT=%%~fI"
set "BUILD_DIR=%ROOT%\build-asi"
set "BUILD_TYPE=Release"

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake was not found in PATH.
    echo Install CMake 3.24 or newer and a C++20 Windows compiler, then retry.
    pause
    exit /b 1
)

cmake -S "%ROOT%" -B "%BUILD_DIR%" -A Win32
if errorlevel 1 goto :configure_failed

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE% --parallel 1
if errorlevel 1 goto :build_failed

echo.
echo ASI build completed successfully.
if exist "%BUILD_DIR%\%BUILD_TYPE%\ReportPanel.asi" echo ASI plugin: "%BUILD_DIR%\%BUILD_TYPE%\ReportPanel.asi"
if exist "%BUILD_DIR%\ReportPanel.asi" echo ASI plugin: "%BUILD_DIR%\ReportPanel.asi"
pause
exit /b 0

:configure_failed
echo.
echo [ERROR] CMake configuration failed. See the messages above.
pause
exit /b 1

:build_failed
echo.
echo [ERROR] Compilation failed. See the messages above.
pause
exit /b 1

