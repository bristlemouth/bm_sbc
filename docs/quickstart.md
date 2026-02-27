# Quickstart

## Prerequisites

- CMake 3.18+
- C17 / C++17 toolchain (GCC or Clang)
- `lib/bm_core` submodule initialized:
  ```
  git submodule update --init --recursive
  ```

## Build

```
cmake -B build -S . -DBM_SBC_BUILD_ALL_APPS=ON
cmake --build build
```

Single app only:
```
cmake -B build -S . -DBM_SBC_APP=example
cmake --build build
```

## Run

Start a node:
```
./build/bm_sbc_example --node-id 0x0000000000000001
```

Start two connected nodes:
```
./build/bm_sbc_multinode --node-id 0x0001 --peer 0x0002 &
./build/bm_sbc_multinode --node-id 0x0002 --peer 0x0001 &
```

Both nodes will discover each other, exchange pings, and log pub/sub traffic.

## Run the tests

```
./scripts/multinode_test.sh
./scripts/gateway_loopback_test.sh   # requires socat
```

## Write an app

Create `apps/myapp/app_main.cpp`:

```cpp
#include <cstdio>

void setup(void) {
  // Called once after the BM stack is initialized.
}

void loop(void) {
  // Called repeatedly (~1 ms cadence).
}
```

Build it:
```
cmake -B build -S . -DBM_SBC_APP=myapp
cmake --build build
./build/bm_sbc_myapp --node-id 0x0000000000000001
```

The stack (neighbor discovery, BCMP, pub/sub, middleware) is fully running
before `setup()` is called. Use `bm_core` APIs directly:

```cpp
extern "C" {
#include "device.h"
#include "messages/ping.h"
}
#include "pubsub.h"
#include "messages/neighbors.h"
```

See `apps/multinode/app_main.cpp` for a working example with neighbor
callbacks, pub/sub, and ping.

