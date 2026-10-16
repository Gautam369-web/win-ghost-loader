@echo off
setlocal

echo ============================================
echo  Building win-ghost-loader (Injector.exe)
echo ============================================

where windres >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] 'windres' not found in PATH. Ensure MinGW-w64 bin is in PATH.
    pause
    exit /b 1
)

where g++ >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] 'g++' not found in PATH. Ensure MinGW-w64 bin is in PATH.
    pause
    exit /b 1
)

echo [1/2] Compiling UAC manifest resource...
windres Injector.rc -o Injector_res.o
if %ERRORLEVEL% NEQ 0 (
    echo [FAIL] Resource compilation failed.
    pause
    exit /b 1
)
echo [OK] Manifest compiled.

echo [2/2] Compiling Injector.exe...
g++ -O2 -o Injector.exe Injector.cpp Injector_res.o -lkernel32 -lpsapi -luser32 -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic -mwindows
if %ERRORLEVEL% NEQ 0 (
    echo [FAIL] Injector compilation failed.
    pause
    exit /b 1
)

del Injector_res.o 2>nul
echo [OK] Build complete: Injector.exe
echo ============================================
pause
