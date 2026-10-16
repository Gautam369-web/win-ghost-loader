# win-ghost-loader

A lightweight, stealthy Win32 process injector and dual-stage DLL dropper that runs without taskbar presence or visible windows.

## Overview

`win-ghost-loader` executes and persists a companion payload DLL (`ScreenCapture.dll`) using a dual-mode fallback strategy:

1. **Primary Strategy (`explorer.exe` Injection)**:
   - Locates `explorer.exe` via Toolhelp32 process snapshotting.
   - Verifies if the target DLL is already loaded using `K32EnumProcessModules`.
   - Allocates remote memory (`VirtualAllocEx`), writes the absolute DLL path (`WriteProcessMemory`), and invokes `LoadLibraryW` via `CreateRemoteThread`.
2. **Fallback Strategy (`rundll32.exe` Host)**:
   - If direct injection is blocked (e.g., modern Windows Exploit Protection, PPL, or security software), it spawns a hidden instance of Microsoft's `rundll32.exe` invoking the DLL's exported `StartHook` routine with `CREATE_NO_WINDOW`.
   - The export maintains a message loop, keeping `rundll32` alive in the background without creating a window or taskbar icon.
3. **Silent Self-Termination**:
   - The injector terminates itself immediately upon handoff, leaving zero leftover injector processes in Task Manager.

## Project Structure

```
win-ghost-loader/
├── Injector.cpp        # Main C++ source for the silent loader
├── Injector.manifest   # UAC elevation manifest (requireAdministrator)
├── Injector.rc         # Resource script to embed the manifest
├── build.bat           # MinGW-w64 build script
└── README.md           # Documentation
```

## Prerequisites

- **OS**: Windows 10 / Windows 11 (x64)
- **Compiler**: MinGW-w64 (`g++` and `windres`)

## Building

Run `build.bat` or compile directly from terminal:

```cmd
windres Injector.rc -o Injector_res.o
g++ -O2 -o Injector.exe Injector.cpp Injector_res.o -lkernel32 -lpsapi -luser32 -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic -mwindows
```

## Disclaimer

This codebase is provided strictly for educational, security research, and administrative automation purposes.
