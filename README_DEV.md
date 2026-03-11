### Development in CLion

This project is optimized for a great experience in CLion.

#### Run Configurations
The following configurations are pre-defined in `.idea/runConfigurations`:

1.  **Limonitor (Daemon)**: Runs the application in background/service mode with the HTTP server active. Useful for testing the Web UI and API.
2.  **Limonitor (Interactive TUI)**: Runs the application with the ncurses-based terminal interface. (Note: Enable "Emulate terminal in output console" in CLion if it's not already enabled).
3.  **Limonitor Unit Tests**: Executes the modular unit test suite (`limonitor_unit_test`).
4.  **Limonitor Integration Test**: Runs the integration tests that require more system context (e.g., BLE or Serial availability).

#### Building
- **Debug**: Standard development profile.
- **Release**: Use this for production builds. It enables hyper-optimizations specifically detected for your hardware (Apple Silicon or Raspberry Pi).

#### Testing
The project uses a custom modular testing framework. You can run all tests via the **Limonitor Unit Tests** configuration, or run individual test targets if defined in `CMakeLists.txt`.

#### Optimization Visibility
When you reload the CMake project, check the **CMake** tab in CLion. It will display a prominent status block confirming which **HYPER-OPTIMIZATION PROFILE** is active (e.g., Apple Silicon) and which compiler flags are being applied.
