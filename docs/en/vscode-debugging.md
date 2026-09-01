[中文](../zh/vscode-debugging.md)

# VS Code and GDB debugging

The repository keeps reusable VS Code/GDB examples under `docs/examples/.vscode/` instead of forcing machine-specific editor settings into the repository root.

## Recommended flow

1. Configure a `Debug` target cross build.
2. Build `canopen_master` with debug symbols.
3. Deploy through the `debug-deploy` CMake target so the project launches the target-side gdbserver path.
4. Point the local debugger at the cross GDB that matches the Yocto SDK and the locally built binary.
5. Load the project example scripts only after adjusting machine-specific paths.

```sh
cmake -S . -B build-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-debug --parallel
cmake --build build-debug --target debug-deploy
```

## Example files

```text
docs/examples/.vscode/c_cpp_properties.json
docs/examples/.vscode/launch.json
docs/examples/.vscode/settings.json
docs/examples/.vscode/load-lely-symbols.gdb
docs/examples/.vscode/skip-system-libs.gdb
```

Do not copy absolute SDK/sysroot paths from another machine without verification.

## Detailed reference

The earlier Chinese guide is retained at `../zh/reference/vscode-debugging.md`.
