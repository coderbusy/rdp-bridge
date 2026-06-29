# RdpBridge

A production-grade native RDP SDK built on [FreeRDP](https://www.freerdp.com/),
exposing a clean **C ABI** suitable for P/Invoke from C#, Avalonia, and other
languages.

## Features

- **Session lifecycle** – create, connect, disconnect, destroy
- **BGRA32 framebuffer callbacks** – zero-copy raw pixel delivery
- **Input** – mouse (move / click / wheel), keyboard scancode, Unicode input
- **Security profiles** – automatic NLA → TLS → RDP → negotiate fallback
- **Thread-safe** – connection on a dedicated worker thread, atomic session state
- **Cross-platform** – Windows (MSVC → `RdpBridge.dll`) and Linux (GCC/Clang → `libRdpBridge.so`)

---

## Quick Start

### Build (Linux)

```bash
sudo apt-get install -y build-essential cmake ninja-build \
    libssl-dev libfreerdp3-dev libwinpr3-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Build (Windows)

```powershell
# Prerequisites: Visual Studio 2022, CMake, vcpkg
vcpkg install freerdp:x64-windows openssl:x64-windows

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

---

## C API

```c
#include "rdp_bridge.h"

void* session = RdpBridge_create();

RdpBridge_set_callbacks(session, on_frame, on_status, on_disconnect, user_data);

RdpBridge_connect(session, "192.168.1.1", 3389, "Administrator", "password", 1920, 1080);

// ... run your event loop ...

RdpBridge_disconnect(session);
RdpBridge_destroy(session);
```

Full API reference: [`include/rdp_bridge.h`](include/rdp_bridge.h)

---

## Repository Layout

```
/include            Public header (rdp_bridge.h)
/src/RdpBridge      Core implementation (rdp_bridge.cpp)
/cmake              (reserved for future CMake helpers)
/examples/minimal   Minimal C usage example
/.github/workflows  CI/CD (build.yml)
```

---

## CI / CD

| Event | Action |
|---|---|
| Push to `main` | Build Linux SO + Windows DLL |
| Tag `v*` | Build + package + upload to GitHub Release |

---

## License

This project is derived from
[CxShell/CxRdpBridge](https://github.com/xiaochengzjc/CxShell/tree/master/native/CxRdpBridge).
See [LICENSE](LICENSE) for details.