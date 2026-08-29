# LockFreeTripleBuffer

[![CI](https://github.com/emreoztr/LockFreeTripleBuffer/actions/workflows/ci.yml/badge.svg)](https://github.com/emreoztr/LockFreeTripleBuffer/actions/workflows/ci.yml)
[![Standard](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE.txt)
[![Header Only](https://img.shields.io/badge/Header--Only-Yes-green.svg)](include/LockFreeTripleBuffer.h)
[![C++20 Module](https://img.shields.io/badge/C%2B%2B20_Module-Yes-green.svg)](modules/LockFreeTripleBuffer.ixx)

A high-performance, cache-aligned, single-producer single-consumer (SPSC) **Lock-Free Triple Buffer** implemented in modern C++20. Available both as a header-only library and as a C++20 named module.

---

## Table of Contents
- [Overview](#overview)
- [Why Triple Buffering?](#why-triple-buffering)
- [Key Features](#key-features)
- [Architecture & Mechanics](#architecture--mechanics)
- [Usage Scenarios](#usage-scenarios)
- [Quick Start](#quick-start)
  - [1. Non-Blocking Read (`ConsumeMode::Try`)](#1-non-blocking-read-consumemodetry)
  - [2. Blocking Wait (`ConsumeMode::Wait`) with `std::jthread`](#2-blocking-wait-consumemodewait-with-stdjthread)
  - [3. C++20 Module Import](#3-c20-module-import)
- [CMake Integration](#cmake-integration)
- [Building and Running Tests](#building-and-running-tests)
- [Acknowledgment](#acknowledgment)
- [License](#license)

---

## Overview

In multi-threaded real-time applications (such as graphics engines, physics simulations, robotics, and audio streaming), producers and consumers frequently operate at drastically different frequencies. For example, a physics or sensor simulation may produce telemetry at 1000 Hz, while a display renderer consumes frames at 60 Hz or 144 Hz.

Traditional FIFO queues require synchronization, introduce latency, and can overflow if the consumer falls behind. Double buffering eliminates memory re-allocation but still blocks or risks frame tearing if both threads access buffers simultaneously.

`LockFreeTripleBuffer` resolves this by providing **three distinct buffers**:
1. **Producer buffer**: Exclusively written to by the producer.
2. **Consumer buffer**: Exclusively read from by the consumer.
3. **Shared buffer**: Atomically swapped between producer and consumer without mutexes or lock overhead.

The consumer is guaranteed to always receive the **freshest available complete frame**, with zero torn reads and zero mutex contention.

---

## Why Triple Buffering?

| Feature | Mutex-Guarded Queue | Lock-Free FIFO Queue | Double Buffer | Lock-Free Triple Buffer |
| :--- | :--- | :--- | :--- | :--- |
| **Lock-Free** | ❌ (Blocks threads) | ✔️ | ❌ / ⚠️ | **✔️ (100% Lock-Free)** |
| **Decoupled Frequencies** | ⚠️ (Queue builds up) | ⚠️ (Requires dropping/overflow) | ⚠️ (May stall producer) | **✔️ (Producer never stalls)** |
| **Freshest Data Semantics**| ❌ (Reads old queue items first) | ❌ (Reads oldest queued items) | ✔️ | **✔️ (Always newest data)** |
| **Zero Torn Reads** | ✔️ | ✔️ | ⚠️ | **✔️ (Acquire/Release CAS)** |
| **Dynamic Allocation** | ❌ (Per node/packet) | ❌ / ⚠️ | ✔️ | **✔️ (Zero allocation at runtime)** |

---

## Key Features

- **Standard C++20 Implementation**: Leverages `std::atomic<State>`, `std::atomic::wait`, `std::atomic::notify_one`, and bitfield CAS exchanges.
- **Cache-Line Alignment & Anti-False-Sharing**: Internal buffers and thread state variables are decorated with `alignas(hardware_destructive_interference_size)` to prevent false sharing across CPU cores.
- **Two Flexible Consumption Modes**:
  - `ConsumeMode::Try`: 100% non-blocking. If no new frame has been published since the last read, returns `false` immediately.
  - `ConsumeMode::Wait`: Kernel-level efficient wait. If no new data is present, puts the consumer thread to sleep using OS primitives (`futex` on Linux, `WaitOnAddress` on Windows) until `notify_one()` is signaled by the producer.
- **Dual Delivery**:
  - **Header-Only**: `#include "LockFreeTripleBuffer.h"` (target: `LockFreeTripleBuffer`).
  - **C++20 Named Module**: `import LockFreeTripleBuffer;` (target: `LockFreeTripleBufferModule`).
- **Zero Heap Allocations**: All 3 buffers are pre-allocated in-place within a contiguous `std::array<T, 3>`.

---

## Architecture & Mechanics

### Internal State Layout
The shared synchronization state is packed into an atomic 1-byte struct:

```cpp
struct alignas(1) State {
    uint8_t stateIndex : 2;  // Current shared buffer index (0, 1, or 2)
    uint8_t hasNewData  : 1;  // 1 if producer published new data, 0 if consumed
    uint8_t reserved    : 5;
    bool operator==(const State&) const = default;
};
```

Because `sizeof(State) == 1`, `std::atomic<State>::is_always_lock_free` evaluates to `true` on modern CPU architectures (using standard 8-bit atomic instructions).

### Swap State Diagram

```
 Producer writes to      ──► [Producer Buffer]
 Buffer[_producerState]               │
                                      │ (Exchange on produce)
                                      ▼
                             [Shared Buffer] ◄── std::atomic<State>
                                      ▲
                                      │ (Exchange on consume)
 Consumer reads from     ◄── [Consumer Buffer]
 Buffer[_consumerState]
```

1. **Produce Step**:
   - Producer writes data into its private buffer index (`_producerState.stateIndex`).
   - Sets `hasNewData = 1`.
   - Atomically swaps `_producerState` with `_sharedState` using `std::memory_order_release`.
   - Producer acquires the previous shared buffer index to use for the next write.
   - Calls `_sharedState.notify_one()` to wake any sleeping consumer.

2. **Consume Step**:
   - Consumer inspects `_sharedState`:
     - If `hasNewData == 0` and `ConsumeMode::Try`: returns `false` immediately.
     - If `hasNewData == 0` and `ConsumeMode::Wait`: enters `_sharedState.wait()` until notified.
   - Atomically swaps `_consumerState` (`hasNewData = 0`) with `_sharedState` using `std::memory_order_acq_rel`.
   - Consumer copies or moves the data from its newly acquired buffer index.

---

## Usage Scenarios

- **Game Engines & Rendering**: Physics runs at 240+ Hz updating positions; rendering thread samples the latest pose at 60/144 Hz without blocking the physics loop.
- **Sensor Acquisition & Robotics**: High-frequency hardware sampling (e.g., IMU at 1000 Hz) sending latest orientation to motion planning.
- **Audio & DSP Processing**: Real-time audio threads streaming parameters to/from visualization or GUI threads.
- **Telemetry Streaming**: Pushing real-time telemetry metrics to UI monitors where intermediate discarded values are acceptable and freshness is critical.

---

## Quick Start

### 1. Non-Blocking Read (`ConsumeMode::Try`)

```cpp
#include "LockFreeTripleBuffer.h"
#include <iostream>

int main() {
    LockFreeTripleBuffer<int> buffer;

    int value = 0;
    // Initial read returns false because nothing has been produced yet
    if (!buffer.consume<ConsumeMode::Try>(value)) {
        std::cout << "Buffer is empty.\n";
    }

    // Producer produces new data
    buffer.produce(42);

    // Consumer reads the latest data
    if (buffer.consume<ConsumeMode::Try>(value)) {
        std::cout << "Received value: " << value << "\n"; // Prints 42
    }

    // Subsequent read returns false until new data is produced
    if (!buffer.consume<ConsumeMode::Try>(value)) {
        std::cout << "No newer data available.\n";
    }

    return 0;
}
```

### 2. Blocking Wait (`ConsumeMode::Wait`) with `std::jthread`

```cpp
#include "LockFreeTripleBuffer.h"
#include <chrono>
#include <iostream>
#include <thread>

struct FrameData {
    uint64_t frameIndex{0};
    double timestamp{0.0};
};

int main() {
    LockFreeTripleBuffer<FrameData> frameBuffer;

    // Consumer thread using C++20 std::jthread
    std::jthread consumerThread([&](std::stop_token stopToken) {
        FrameData frame;
        while (!stopToken.stop_requested()) {
            // Sleeps efficiently on OS futex / WaitOnAddress until producer notifies
            if (frameBuffer.consume<ConsumeMode::Wait>(frame)) {
                std::cout << "Rendering frame #" << frame.frameIndex << "\n";
                if (frame.frameIndex == 100) break;
            }
        }
    });

    // Producer thread
    for (uint64_t i = 1; i <= 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frameBuffer.produce(FrameData{i, 0.005 * i});
    }

    return 0;
}
```

### 3. C++20 Module Import

```cpp
import LockFreeTripleBuffer;
#include <iostream>

int main() {
    LockFreeTripleBuffer<double> buffer;
    buffer.produce(3.14159);

    double value = 0.0;
    if (buffer.consume<ConsumeMode::Try>(value)) {
        std::cout << "Imported module received: " << value << "\n";
    }
}
```

---

## CMake Integration

### Option A: `FetchContent` (Recommended)

```cmake
include(FetchContent)

FetchContent_Declare(
    LockFreeTripleBuffer
    GIT_REPOSITORY https://github.com/emreoztr/LockFreeTripleBuffer.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(LockFreeTripleBuffer)

# Link header-only target
target_link_libraries(my_project PRIVATE LockFreeTripleBuffer)

# Or link C++20 module target (when using Ninja or Visual Studio generator)
# target_link_libraries(my_project PRIVATE LockFreeTripleBufferModule)
```

### Option B: Subdirectory

```cmake
add_subdirectory(LockFreeTripleBuffer)
target_link_libraries(my_project PRIVATE LockFreeTripleBuffer)
```

---

## Building and Running Tests

The project includes an automated test suite powered by **GoogleTest (v1.14.0)**, covering:
1. `ConsumeMode::Try` non-blocking semantics and overwrite tests.
2. `ConsumeMode::Wait` sleep and wake-up synchronization tests.
3. Concurrent 500,000-packet multi-threaded stress test with 64-bit checksum data integrity and strict monotonic ordering checks.

### On Windows (Visual Studio / MSVC)
```powershell
# Configure with CMake (auto-detects Visual Studio 2022/2026)
cmake -B build -A x64

# Build all targets in Release mode
cmake --build build --config Release

# Run tests via CTest
ctest --test-dir build -C Release --output-on-failure --verbose
```

### On Linux (GCC or Clang with Ninja)
```bash
# Configure with Ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build all targets
cmake --build build

# Run tests via CTest
ctest --test-dir build --output-on-failure --verbose
```

---

## Acknowledgment

- **Core Algorithm & Concept**: Designed and implemented by [Yunus Emre Öztürk](https://github.com/emreoztr).
- **Tooling & Infrastructure**:
  - The CMake 3.28+ build system (`INTERFACE` and `FILE_SET CXX_MODULES` targets),
  - C++20 module interface wrapper ([`LockFreeTripleBuffer.ixx`](modules/LockFreeTripleBuffer.ixx)),
  - Unit test suite and 500,000-packet concurrent stress tests ([`tests/test_main.cpp`](tests/test_main.cpp)),
  - And the multi-platform GitHub Actions CI/CD pipeline ([`.github/workflows/ci.yml`](.github/workflows/ci.yml))
  were co-developed with AI assistance (Google Antigravity) and reviewed, validated, and approved by the author.

---

## License

This project is licensed under the Apache License 2.0. See the [LICENSE.txt](LICENSE.txt) file for details.

