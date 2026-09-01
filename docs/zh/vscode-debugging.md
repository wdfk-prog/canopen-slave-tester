[English](../en/vscode-debugging.md)

# VS Code 与 GDB 调试

仓库把可复用的 VS Code/GDB 示例放在 `docs/examples/.vscode/`，避免把开发机绝对路径直接放到仓库根目录的个人 editor 配置中。

## 推荐流程

1. 配置 `Debug` 目标交叉构建；
2. 编译带完整调试符号的 `canopen_master`；
3. 使用 `debug-deploy` CMake target 部署并启动目标端 gdbserver；
4. 本机 debugger 使用与 Yocto SDK 匹配的 cross GDB，并指向本机构建出的同一二进制；
5. 根据本机实际 SDK/sysroot 路径调整示例配置后再加载。

```sh
cmake -S . -B build-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-debug --parallel
cmake --build build-debug --target debug-deploy
```

## 示例文件

```text
docs/examples/.vscode/c_cpp_properties.json
docs/examples/.vscode/launch.json
docs/examples/.vscode/settings.json
docs/examples/.vscode/load-lely-symbols.gdb
docs/examples/.vscode/skip-system-libs.gdb
```

不要直接复制其他机器的绝对 SDK/sysroot 路径而不做复核。

## 详细参考

旧版中文说明保留在 [`reference/vscode-debugging.md`](reference/vscode-debugging.md)。
