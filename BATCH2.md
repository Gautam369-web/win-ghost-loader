# ScreenCapture improvements — 12 October 2026

Subsequent update: the DLL now sends one `model: "auto"` request to Veda. See `veda-dashboard/AI_CONTROL.md` for the four-model catalog, verification requirements and quota routing.

This report supersedes the remaining-work list in `IMPROVEMENTS.md`. Original source backups are in `review_backup`.

## Implemented

- Strict JSON parsing with vendored nlohmann/json 3.12.0 (MIT license in `third_party`). Rejects duplicate keys, malformed/truncated JSON, invalid Unicode, excessive depth and oversized input. Root-level configuration values cannot be confused with nested values.
- HTTPS validates certificates normally, checks HTTP status, bounds response size, and rejects failed/incomplete reads. RAII closes handles. Calls check cancellation and deadlines between blocking operations.
- Only complete Gemini answers (`finishReason: STOP`) are accepted; thought parts are excluded. Configuration saves preserve unknown fields and refuse to overwrite malformed files. Leading C preprocessor characters are retained.
- Overlay rendering uses a snapshot so GDI drawing does not hold the shared state lock. Failure updates respect request cancellation. History carries subject and whether an answer can be typed.
- Clipboard publication has a valid owner window and checks allocation/open/empty/set failures. **Ctrl+Shift+C** explicitly copies the current answer. Workers no longer automatically overwrite the clipboard.
- **F9 now displays an MCQ suggestion for manual review and selection.** It includes extracted question text, accepts only a single A–D answer, clears the typing buffer, and performs no mouse movement/click or corner gesture.
- Veda integration sends device ID and uses protocol 2. See `veda-dashboard/REVIEW.md` for API changes, server setup, compatibility and tests. Provider credentials stay on the server.

## Build

`build_review.sh` uses the supplied C++17/O2/linker flags but now outputs **ScreenCapture.review.dll** to preserve both existing `ScreenCapture.dll` and `ScreenCapture2.dll`.

```powershell
rtk proxy C:\msys64\usr\bin\bash.exe --login /c/Users/Rossie/Downloads/ScreenCapture/test_review.sh
rtk proxy C:\msys64\usr\bin\bash.exe --login /c/Users/Rossie/Downloads/ScreenCapture/build_review.sh
```

The regression executable exercises actual source helpers with fake HTTP and clipboard adapters. 557 assertions include 500 repeated concurrent-buffer snapshots; this is not 557 independent scenarios. It installs no hooks, captures no desktop, sends no input, uses no live clipboard and makes no network calls.

## Deployment and limitations

The new DLL and Veda server require a coordinated rollout. Neither has been deployed. After staging the updated server, request Alt+Q and wait for the host to finish unloading before replacing any DLL. Shutdown may wait for a blocked UI Automation provider or synchronous WinINet call; a deadline is not an immediate OS-call cancellation. Never force-unload executing code. Keep the existing DLLs for rollback.

The review does not verify actual injected-host behavior, keyboard focus/input delivery, UI Automation coverage, multiple monitors/DPI, or overlay scrolling. The keyboard hook still performs buffer/history work and uses shared synchronization; moving all actions into a dedicated event loop and binding typing to an explicitly chosen foreground window remain architectural improvements. API models must remain available to the configured provider account.
