# win-ghost-loader

> A stealthy Windows x64 DLL payload system featuring AI-assisted text extraction, MCQ solving, a floating glassmorphic overlay, and a zero-window silent injector — all built in pure C++17 with no runtime dependencies beyond MinGW-w64.

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Project Structure](#project-structure)
- [Features](#features)
- [Hotkeys](#hotkeys)
- [Requirements](#requirements)
- [Building](#building)
- [Configuration](#configuration)
- [How It Works](#how-it-works)
  - [Startup Flow](#startup-flow)
  - [AI Answer Flow (F8)](#ai-answer-flow-f8)
  - [MCQ Review Flow (F9)](#mcq-review-flow-f9)
  - [Stealth Typing (F6)](#stealth-typing-f6)
  - [Mouse Movement](#mouse-movement)
  - [Thread Safety](#thread-safety)
- [Testing](#testing)
- [File Reference](#file-reference)
- [Third-Party Credits](#third-party-credits)
- [Disclaimer](#disclaimer)

---

## Overview

`win-ghost-loader` consists of two executable components and one core DLL payload:

| Component | File | Role |
|---|---|---|
| **Silent Injector** | `Injector.exe` | Injects `ScreenCapture.dll` into `explorer.exe` with zero visible footprint |
| **Direct Launcher** | `Veda.exe` | Loads `ScreenCapture.dll` directly with mutex lock and friendly error dialogs |
| **Payload DLL** | `ScreenCapture.dll` | All features: overlay, AI, text extraction, stealth typing, MCQ solving |

The DLL installs a **low-level keyboard hook** (`WH_KEYBOARD_LL`), captures text from the foreground window using **UI Automation + DirectX/DXGI screen capture**, forwards questions to **Google Gemini AI** via a proxy server, and renders answers in a **borderless always-on-top overlay**.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Client Machine                           │
│                                                                 │
│  Veda.exe ──LoadLibraryExW──► ScreenCapture.dll                │
│      OR                            │                           │
│  Injector.exe ─RemoteThread──►  explorer.exe ◄─── hook alive  │
│                                    │                           │
│                        ┌───────────▼──────────────┐            │
│                        │   WH_KEYBOARD_LL hook     │            │
│                        │   + Overlay Window        │            │
│                        │   + Worker Thread Pool    │            │
│                        └─────────────┬─────────────┘            │
│                                      │                          │
│                              F8/F9 hotkey                       │
│                                      │                          │
│                     ┌────────────────▼──────────────────┐       │
│                     │  UI Automation (IUIAutomation)     │       │
│                     │  + DXGI/BitBlt screen capture      │       │
│                     └────────────────┬──────────────────┘       │
└──────────────────────────────────────┼──────────────────────────┘
                                       │ HTTPS (WinINet)
                          ┌────────────▼──────────────┐
                          │   vedax.vercel.app         │
                          │   /api/verify   (HWID)     │
                          │   /api/config   (models)   │
                          │   /api/ai       (prompt)   │
                          └────────────┬──────────────┘
                                       │
                          ┌────────────▼──────────────┐
                          │   Google Gemini API        │
                          │   (keys never leave        │
                          │    the proxy server)       │
                          └───────────────────────────┘
```

---

## Project Structure

```
win-ghost-loader/
│
├── ScreenCaptureDLL.cpp      # Main DLL — all features (~1800 lines, C++17)
├── ScreenCaptureSupport.h    # namespace sc:: — response cleaning, newline norm
├── HttpSupport.h             # RAII WinINet HTTPS client, deadline + cancel aware
├── JsonSupport.h             # Minimal JSON string parser helpers (sc:: namespace)
├── ClipboardSupport.h        # RAII Win32 clipboard open/set/close helpers
│
├── Injector.cpp              # Silent remote-thread DLL injector (no window)
├── Injector.manifest         # UAC: requireAdministrator
├── Injector.rc               # Resource script to embed the manifest
│
├── VedaLauncher.cpp          # Direct launcher (Veda.exe) with mutex + error UI
│
├── regression_tests.cpp      # Full unit test suite (564 assertions, no hooks/net)
├── http_smoke.cpp            # Live HTTP smoke test against /api/verify
│
├── build_review.sh           # Build ScreenCapture.review.dll (MinGW-w64 bash)
├── build_launcher.sh         # Build Veda.exe (MinGW-w64 bash)
├── test_review.sh            # Build + run regression_tests.exe
├── test_http.sh              # Build + run http_smoke.exe
│
├── IMPROVEMENTS.md           # Batch 1 stability changelog (Oct 10)
├── BATCH2.md                 # Batch 2 changelog (Oct 12) — Protocol 2, nlohmann JSON
│
└── third_party/
    ├── json.hpp              # nlohmann/json 3.12.0 (single-header, MIT)
    └── LICENSE.json.MIT      # MIT license for nlohmann/json
```

---

## Features

### 🤖 AI Answer Engine
- Sends the text under the cursor to **Google Gemini** via the Veda proxy server.
- Supports **8 subjects** with tailored system prompts (Python, Java, C, C++, DBMS, Analytical, Communication Skills, Others).
- Cycle subjects with **Shift+F8**.
- AI responses cleaned of Markdown fences, preserving code indentation.
- Answers stored in `VedaData/qa/` next to the DLL.

### 📝 MCQ Solver (F9)
- Extracts the full question and options via **UI Automation** from the foreground element.
- Sends to Gemini, receives a single letter `A–D`.
- Moves the mouse in a **human-like Bezier curve** to the matching Z-pattern screen corner as a visual cue:
  - **A** → Top-Left
  - **B** → Top-Right
  - **C** → Bottom-Left
  - **D** → Bottom-Right
- Results saved to `VedaData/mcq/`.

### ⌨️ Stealth Typing Buffer
- AI answer is loaded silently into a typed buffer.
- Press **F6** once to enable — each subsequent F6 injects the next Unicode character via `SendInput(KEYEVENTF_UNICODE)`.
- Handles multi-byte Unicode, CJK, emoji, CRLF normalization.
- **F6 × 2 fast** = reset buffer to start.
- **F12** = reset buffer index to 0.
- **Ctrl+F6** = load clipboard content into buffer.

### 🖥️ Always-On-Top Overlay
- Borderless, always-on-top, glassmorphic floating window.
- Three modes:
  - **Mode 0**: Shortcut cheatsheet
  - **Mode 1**: AI answer or MCQ suggestion
  - **Mode 2**: Live processing tracker with spinner (Extract → AI → Done)
- **F7** toggles show/hide.
- **Ctrl+Shift+C** copies current answer to clipboard.
- Answer history (up to 50 entries): **F1** = previous, **F2** = next.

### 📸 Screenshot Engine
- **Alt+S**: Captures full virtual screen (all monitors) using `BitBlt`.
- Saved as PNG to `%USERPROFILE%\Pictures\Screenshots\`.

### 🔒 HWID Verification (Protocol 2)
- On startup reads `user_config.json` for `name` and `registration_number`.
- Computes `hwid = USERNAME_COMPUTERNAME` and `deviceId = COMPUTERNAME_VolumeSerial`.
- Calls `GET /api/verify` — server creates/checks the device binding.
- Fetches `/api/config` for active model list.
- All AI calls proxied through `/api/ai` — **Gemini API keys never touch the client**.

### 🖱️ Human-Like Mouse Movement
- Cubic Bezier path with two random control points.
- Smooth ease-in/ease-out (smoothstep).
- Micro-jitter that fades to zero near the target.
- LCG random seed seeded with `GetTickCount() XOR cursor position` — no two moves are identical.
- Variable per-step delay: 4–14ms, peaking at midpoint, with ±4ms noise.

---

## Hotkeys

| Key | Action |
|---|---|
| **F8** | Extract text under cursor → send to Gemini AI → show answer + load typing buffer |
| **Shift+F8** | Cycle subject (Python → Java → C → C++ → DBMS → Analytical → PEL → Others) |
| **F9** | Extract MCQ → AI → suggest letter → move mouse to Z-corner |
| **F6** | Toggle stealth typing ON/OFF; each press types one character from buffer |
| **F6 × 2** (fast) | Reset buffer index to start |
| **F12** | Reset buffer index to 0 |
| **F1** | Previous answer in history (disables typing) |
| **F2** | Next answer in history (disables typing) |
| **F7** | Toggle overlay show/hide |
| **Ctrl+F6** | Load clipboard content into stealth typing buffer |
| **Ctrl+Shift+C** | Copy current displayed answer to clipboard |
| **Alt+S** | Take fullscreen screenshot → save as PNG |
| **Alt+Q** | Graceful shutdown — drains all background workers, releases DLL |

---

## Requirements

- **OS**: Windows 10 / Windows 11 (x64)
- **Compiler**: MinGW-w64 with GCC ≥ 11 (`g++`) and `windres`
- **Shell**: MSYS2 bash for `.sh` build scripts (`C:\msys64\usr\bin\bash.exe`)
- **Runtime**: Static — no MSVC redistributable needed. Distributes with `libwinpthread-1.dll` only.

---

## Building

### DLL (`ScreenCapture.dll`)

```bash
# From MSYS2 bash:
./build_review.sh
# Output: ScreenCapture.review.dll
```

Or manually:

```bash
export PATH=/mingw64/bin:/usr/bin:$PATH
g++ -std=c++17 -O2 -shared -o ScreenCapture.dll ScreenCaptureDLL.cpp \
    -lgdi32 -lgdiplus -ldwmapi -ld3d11 -ldxgi \
    -lole32 -loleaut32 -luuid \
    -luser32 -lkernel32 -lpsapi -lwininet -lcomctl32 \
    -static-libgcc -static-libstdc++ \
    -Wl,-Bstatic -lpthread -Wl,-Bdynamic
```

### Launcher (`Veda.exe`)

```bash
./build_launcher.sh
```

Or manually:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -municode -mwindows -static \
    -o Veda.exe VedaLauncher.cpp -luser32 -lkernel32
```

### Injector (`Injector.exe`)

```cmd
windres Injector.rc -o Injector_res.o
g++ -O2 -mwindows -o Injector.exe Injector.cpp Injector_res.o ^
    -lkernel32 -lpsapi -luser32 ^
    -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic
```

### Obfuscated / Strip Build (for distribution)

```bash
g++ -std=c++17 -O3 -s -fvisibility=hidden -shared \
    -o ScreenCapture.dll ScreenCaptureDLL.cpp \
    [same libs as above]
# Then optionally: upx --best --lzma ScreenCapture.dll
```

---

## Configuration

Place `user_config.json` in the **same folder** as `ScreenCapture.dll`:

```json
{
  "registration_number": "21BCE1234",
  "name": "Your Name",
  "default_language": "Python"
}
```

| Field | Required | Description |
|---|---|---|
| `registration_number` | Yes | Your student/user registration ID sent to the Veda server |
| `name` | Yes | Your display name sent during HWID verification |
| `default_language` | No | Default subject on startup (`Python`, `Java`, `C`, `C++`, `DBMS`, etc.) |

> **Never commit `user_config.json` to version control.** It contains personal identity data.

---

## How It Works

### Startup Flow

```
DLL loaded (DllMain or StartHook)
    │
    ├─ Read user_config.json
    ├─ GET /api/verify?hwid=&deviceId=&name=&reg=
    │       ← {status: "pending"|"approved"|"blocked", expiry, protocol:2}
    │
    ├─ If approved:
    │   └─ GET /api/config  ← {models: ["models/gemini-2.5-flash", ...]}
    │
    ├─ Start overlay thread (glassmorphic window, Mode 0 = shortcuts)
    └─ Install WH_KEYBOARD_LL hook → enter GetMessage loop
```

### AI Answer Flow (F8)

```
F8 pressed
    │
    ├─ Increment g_extractGen (cancels any prior F8/F9 thread)
    ├─ Capture: foreground HWND + cursor position (thread_local snapshot)
    │
    ├─ [Worker Thread]
    │   ├─ UI Automation: walk element tree under cursor → extract text
    │   ├─ If empty: BitBlt screenshot fallback
    │   ├─ POST /api/ai {prompt, model:"auto", hwid, deviceId}
    │   │       ← Gemini response JSON
    │   ├─ Clean response (strip outer Markdown fence)
    │   ├─ SetAnswer → overlay Mode 1, load stealth typing buffer
    │   └─ Save to VedaData/qa/qa_YYYYMMDD_HHMMSS.txt
```

### MCQ Review Flow (F9)

```
F9 pressed
    │
    ├─ [Worker Thread]
    │   ├─ UI Automation extract question + options text
    │   ├─ POST /api/ai with MCQ-specific prompt
    │   ├─ Parse single letter A–D from response
    │   ├─ SetAnswer → overlay shows "Suggested option: B"
    │   ├─ MoveMouseHuman → Z-quadrant corner matching the letter
    │   └─ Save to VedaData/mcq/mcq_YYYYMMDD_HHMMSS.txt
```

### Stealth Typing (F6)

```
F6 (typing enabled)
    │
    ├─ g_stealthOn = true; g_bufIdx points to current char in buffer
    │
    └─ Each subsequent F6 press:
        ├─ Read next wchar_t from g_stealthBuf
        ├─ Build INPUT with KEYEVENTF_UNICODE
        └─ SendInput(2, inp, sizeof(INPUT))  (key down + key up)
```

### Mouse Movement

`MoveMouseHuman(tx, ty)` uses a **cubic Bezier curve** $B(t)$:

$$B(t) = (1-t)^3 P_0 + 3(1-t)^2 t \cdot CP_1 + 3(1-t)t^2 \cdot CP_2 + t^3 P_3$$

- $P_0$ = current cursor position, $P_3$ = target position
- $CP_1$, $CP_2$ = random perpendicular deviations (±35% and ±20% of distance)
- Step count: 15–55, proportional to pixel distance
- Timing: sinusoidal speed profile (fast in middle, slow at start/end)
- Jitter: up to ±1.6px at start, fades to zero at target

### Thread Safety

| Mechanism | Protects |
|---|---|
| `g_stateMutex` (`std::recursive_mutex`) | All shared UI state: `g_lastLog`, `g_answerDisplay`, `g_overlayMode`, `g_procStep`, `g_stealthBuf`, `g_answerHistory` |
| `g_workersMutex` (`std::mutex`) | `g_workers` vector |
| `g_configMutex` (`std::mutex`) | `user_config.json` read/write |
| `g_extractGen` (`std::atomic<int>`) | Latest-wins cancellation for F8/F9 |
| `g_stopping` (`std::atomic<bool>`) | Global shutdown signal |
| `t_generation` (`thread_local int`) | Per-thread generation snapshot |
| `StateLock` (`std::lock_guard`) | RAII lock helper used at every shared-state write |

**Latest-wins cancellation**: every new F8 or F9 press increments `g_extractGen`. Background threads check `RequestCancelled()` between each major stage (after extract, after AI call, inside HTTP read loop). Superseded threads self-exit immediately.

---

## Testing

### C++ Unit Tests (564 assertions, no network/hooks)

```bash
./test_review.sh
# Compiles regression_tests.cpp and runs it
# Expected output: PASS: 564 checks; no hooks, capture, input, or network used.
```

Tests cover: buffer indentation logic, Markdown fence stripping, Unicode/JSON escaping, atomic worker supersession, concurrent buffer snapshots, clipboard helpers, MCQ letter parsing, config atomic writes.

### Live HTTP Smoke Test

```bash
./test_http.sh
# Builds http_smoke.exe and hits https://vedax.vercel.app/api/verify
```

Without identity: expects `HTTP 400` (server validates before any DB write).  
With `user_config.json`:
```bash
./http_smoke.exe user_config.json
# Expected: HTTP 200 | Device status=approved
```

---

## File Reference

| File | Purpose | Lines |
|---|---|---|
| `ScreenCaptureDLL.cpp` | Main DLL payload — all runtime logic | ~1800 |
| `ScreenCaptureSupport.h` | `namespace sc::` — `CleanResponse`, `NormalizeNewlines`, `McqLetter` | 32 |
| `HttpSupport.h` | RAII HTTPS client — deadline, cancel, body size limit | 53 |
| `JsonSupport.h` | Lightweight JSON string/value reader (no external dependency) | ~80 |
| `ClipboardSupport.h` | RAII Win32 clipboard API wrapper | ~40 |
| `Injector.cpp` | Silent `explorer.exe` injector with `rundll32` fallback | 135 |
| `VedaLauncher.cpp` | Single-instance launcher with named mutex and error dialogs | 70 |
| `regression_tests.cpp` | Full test suite compiled against real source, no system side-effects | 162 |
| `http_smoke.cpp` | CLI tool to test live `/api/verify` endpoint | 43 |
| `third_party/json.hpp` | nlohmann/json 3.12.0 single-header library | ~25000 |

---

## Third-Party Credits

| Library | Version | License | Usage |
|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | Strict JSON parsing for config files and API responses |

---

## Disclaimer

This codebase is provided strictly for **educational, security research, and administrative automation** purposes. The authors make no warranties about its fitness for any particular use. Usage in environments you do not own or do not have explicit permission to operate in may violate applicable laws. Use responsibly.
