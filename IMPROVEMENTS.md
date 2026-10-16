# Stability improvements — 10 October 2026

This is the historical first-batch report. See `BATCH2.md` and
`veda-dashboard/REVIEW.md` for the current build, API contract and remaining limits.

This batch changes `ScreenCaptureDLL.cpp` and adds `ScreenCaptureSupport.h`.
The original source is preserved in
`review_backup/ScreenCaptureDLL.before-improvements.cpp`.

## Changes

- Background threads are tracked, capped at 16 simultaneous workers, and joined
  during shutdown. Alt+Q rejects new work and invalidates outstanding extraction
  requests before removing the hook and releasing GDI+.
- The Explorer worker releases the DLL's load reference after cleanup. The
  rundll32 entry point returns after cleanup. No teardown runs in DllMain.
- Extraction workers receive the generation, subject, cursor, and foreground
  window captured when the request is accepted. Cancellation is checked between
  traversal, networking, and publishing stages.
- Shared buffer, history, status, and MCQ data use a common mutex. Scalar subject,
  overlay handle, and display mode are atomic. Display updates from other threads
  are posted to the overlay's owning thread.
- Complete outer Markdown fences are removed without changing indentation or
  internal backticks. Plain response text is preserved. The typing buffer no
  longer skips whitespace after newlines; CRLF and CR are normalized to LF.
- Loading a new buffer disables typing until F6 is pressed again. This also
  applies when navigating history. Keyboard event pairs no longer sleep inside
  the hook; failed delivery stops typing without advancing the buffer.
- Configuration writes retain the personal API key, escape JSON values, support
  Unicode paths, flush a temporary file, and replace the destination atomically.
  Failed subject saves leave the current selection unchanged. Startup prevents
  subject changes from overwriting configuration before identity data is ready.

## Build and tests

`build_review.sh` uses the supplied C++17/O2 linker flags and outputs
`ScreenCapture2.dll`. It does not replace `ScreenCapture.dll` or run the injector.

`test_review.sh` builds and runs `regression_tests.cpp`. It exercises real source
helpers and managed workers: nested indentation, code fences, newline handling,
JSON escapes and Unicode, personal-key preservation, atomic replacement failure,
concurrent buffer snapshots, request supersession, and shutdown draining.

Tests do not install hooks, capture the desktop, send keyboard/mouse input,
start an overlay, read the user's configuration, or make network requests.

## Remaining limits

Shutdown waits for workers to return. An already blocked UI Automation provider
or synchronous network call cannot be force-cancelled by this batch; unloading is
delayed instead of freeing code that is still executing. Real host unload/reload,
UI behavior, and network integration require an interactive smoke test.

This is the first stability batch, not completion of every finding in the review.
The existing MCQ decision/click behavior, broader JSON/network handling,
certificate-validation flags, multi-monitor/DPI support, and overlay scrolling
still need separate work. The output is not deployed automatically.
