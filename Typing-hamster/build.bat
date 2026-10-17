@echo off
setlocal
pushd "%~dp0"
where g++ >nul 2>nul
if errorlevel 1 (
    echo [ERROR] g++ was not found. Install MinGW-w64 and check PATH.
    popd
    exit /b 1
)

g++ main.cpp -o TypingPet.exe -std=c++17 -O2 -municode -mwindows -lgdiplus -lgdi32 -lshell32 -Wall -Wextra
if errorlevel 1 (
    echo [ERROR] Build failed. Close a running TypingPet.exe and check the errors above.
    popd
    exit /b 1
)

echo [OK] Built TypingPet.exe. Run .\TypingPet.exe to start.
popd
exit /b 0

