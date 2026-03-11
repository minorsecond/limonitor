### limonitor Hyper-Optimizations

This document describes the hyper-optimizations implemented for different hardware platforms.

#### Compiler-Level Optimizations

The build system (`CMakeLists.txt`) now automatically detects the target architecture and applies the best-known compiler flags for performance and stability.

- **All Platforms (Release mode):**
  - `-O3`: Aggressive optimizations for speed.
  - `-flto`: Link-time optimization to enable cross-file optimizations.
  - `-fomit-frame-pointer`: Frees up a register (especially useful on ARM).

- **Apple Silicon (M1/M2/M3):**
  - `-mcpu=apple-m1`: Optimizes instruction scheduling and pipeline usage specifically for Apple's high-performance cores.

- **Raspberry Pi 3B+ / Zero 2W (Cortex-A53):**
  - **AArch64 (64-bit):**
    - `-march=armv8-a+crc`: Enables ARMv8-A instructions including CRC32 extensions.
    - `-mtune=cortex-a53`: Tunes code generation for the specific pipeline of the Cortex-A53 processor.
  - **ARMv7 (32-bit):**
    - `-march=armv8-a+crc -mtune=cortex-a53`
    - `-mfpu=neon-fp-armv8`: Enables NEON SIMD and hardware floating point optimizations.
    - `-mfloat-abi=hard`: Standard for modern Pi 32-bit OS builds.

- **Generic ARM / Native:**
  - `-march=native`: When building on the target device, the compiler will use all features available on that specific CPU.

#### Code-Level Optimizations

- **Const Correctness:** Improved use of `const` in hot paths (like `AnalyticsEngine`) to allow the compiler to make better assumptions about data stability and perform deeper optimizations.
- **Floating Point Stability:** Avoiding `-ffast-math` by default to ensure reliability in battery calculations, while still achieving high performance through architecture-specific instruction sets.
- **Efficient Math:**
  - Linear regression and trend calculations in `AnalyticsEngine` use incremental sum updates, reducing complexity from O(N) to O(1) per poll. This minimizes CPU spikes on low-power ARM cores like the Cortex-A53 (RPi Zero 2W).
  - Reduced redundant floating point absolute value calls and stabilized trend denominators.

- **Memory Efficiency:**
  - `DataStore` avoids heap allocations in the hot-path notification loop by notifying observers directly within the synchronization lock, eliminating per-poll vector copies.
  - Continued use of fixed-size stack buffers for logging and formatting to avoid heap fragmentation during long-duration emergency operation.

- **Data Stability (Emergency Focus):**
  - SQLite is configured with `PRAGMA synchronous=EXTRA`. While slower than the default `NORMAL`, it provides the strongest guarantee that battery and charger state is flushed to disk and durable even if the system loses power unexpectedly (e.g., solar/battery failure).
  - WAL (Write-Ahead Logging) remains active to allow concurrent dashboard reads without blocking background logging.
