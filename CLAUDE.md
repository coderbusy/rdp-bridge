# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

RdpBridge is a native C++ shared library (`RdpBridge.dll` / `libRdpBridge.so`) that wraps [FreeRDP](https://www.freerdp.com/) and exposes a minimal **C ABI** for P/Invoke from C#, Avalonia, or other languages. There are no .NET projects — the entire codebase is C/C++ built with CMake.

## Build Commands

### Linux

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    libssl-dev freerdp3-dev libwinpr3-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Output: build/libRdpBridge.so
```

### Windows (requires Visual Studio 2022, CMake, vcpkg)

```powershell
vcpkg install freerdp:x64-windows openssl:x64-windows

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
# Output: build/Release/RdpBridge.dll
```

Pass `-DRDPBRIDGE_BUILD_EXAMPLES=ON` to also build `examples/minimal`.

## Architecture

The library is a single translation unit (`src/RdpBridge/rdp_bridge.cpp`) behind a single public header (`include/rdp_bridge.h`).

**Internal data flow:**
1. Caller invokes `RdpBridge_connect()` — this starts a `std::thread` (`connection_thread`).
2. The worker thread tries four FreeRDP security profiles in order: NLA → TLS → RDP-legacy → negotiate. It creates a fresh `freerdp` instance per attempt and tears it down on failure.
3. On success, `rdp_post_connect` initialises the software GDI layer (`gdi_init`) with `PIXEL_FORMAT_BGRA32` and wires up `BeginPaint`/`EndPaint` hooks.
4. `rdp_end_paint` fires on every rendered frame, copies the GDI primary buffer, and delivers it to the caller-supplied `RdpBridge_FrameCallback` under `callback_mutex`.
5. `RdpBridge_disconnect()` sets `running = false`, calls `freerdp_abort_connect`, and joins the worker thread.

**Key structs (all internal, anonymous namespace):**
- `RdpSession` — owns everything: the `freerdp*` instance, the worker thread, atomics for state, the three callbacks, credentials, and the last-error string.
- `RdpContext` — extends `rdpContext` (required by FreeRDP's context-size mechanism) to carry a back-pointer to `RdpSession`.

**Thread safety:** callbacks are guarded by `callback_mutex`. Session state transitions use `std::atomic_bool` (`running`, `connected`).

**OpenSSL providers:** `ensure_openssl_providers()` loads the `default` and `legacy` OpenSSL providers once per session. On Windows it also attempts to pre-load `legacy.dll` natively and searches for an `ossl-modules/` directory next to the DLL.

## CI/CD

`.github/workflows/build.yml` builds on `ubuntu-24.04` (GCC + Ninja) and `windows-2022` (MSVC). Tag pushes (`v*`) also create a GitHub Release with `RdpBridge-linux-x64.tar.gz` and `RdpBridge-windows-x64.zip`, each containing `include/rdp_bridge.h` plus the compiled library.
