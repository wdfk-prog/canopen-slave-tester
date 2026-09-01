[中文](../zh/getting-started.md)

# Getting started

## Prerequisites

For the deployed TQ8MP build you need:

- the project Yocto SDK and its aarch64 cross compiler;
- a target-built Lely installation with headers, shared libraries, and `liblely-coapp.pc`;
- CMake 3.16 or newer;
- a Linux host with access to the target board and CAN tooling as required by your workflow.

## 1. Prepare local cross-build settings

Copy the example and edit only the local copy:

```sh
cp cmake/build_config.local.cmake.example cmake/build_config.local.cmake
```

Configure these values for the development machine:

- `CANOPEN_TOOLCHAIN_BIN_DIR`
- `CANOPEN_TOOLCHAIN_PREFIX`
- `CANOPEN_SYSROOT`
- `CANOPEN_LELY_INCLUDE_DIR`
- `CANOPEN_LELY_LIBRARY_DIR`
- `CACHED_IP_ADDR`

`cmake/build_config.local.cmake` is listed in `.gitignore`; this prevents accidental new tracking but does not untrack an existing Git entry.

## 2. Build for TQ8MP

The default CMake path automatically loads `cmake/build_config.cmake`, which in turn loads the local file above.

```sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel
```

The default single-config build type is `MinSizeRel`. For source debugging:

```sh
cmake -S . -B build-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-debug --parallel
```

## 3. Prepare SocketCAN and the DUT

The checked-in runtime expects:

- Host CAN interface: `can1`;
- expected bitrate: `1000000` bit/s;
- Host master Node-ID: `127`;
- MCU Node-ID: `1`;
- software peer Node-ID: `2` when the slave role is used.

The application verifies the configured CAN bitrate before starting Lely. The interface itself must already exist and be UP.

## 4. Select a validation profile

`include/canopen_config.h` selects the Host role. Individual protocol processes are enabled in their module headers. The checked-in default is Master role with SRDO validation enabled and the other automatic master-side processes disabled.

Only enable a process if the MCU firmware exposes the required CANopen feature and any test-only diagnostic Object Dictionary entries used by that process.

See [Configuration](configuration.md) and [Testing and validation](testing.md).

## 5. Deploy

Create the developer-local deployment settings if required:

```sh
cp deploy/local.conf.example deploy/local.conf
```

Then use the CMake deployment target from a target cross-build directory:

```sh
cmake --build build --target download
```

See [Deployment](deployment.md) before using password-based SSH or modifying the target installation directory.

## Optional native host build

`CANOPEN_NATIVE_BUILD=ON` remains available for explicit local host-side experiments with a compatible native Lely installation. It is not used by CI or Release. CI and Release use the real TQ8MP Yocto SDK on the self-hosted runner described in [CI/CD](ci-cd.md).
