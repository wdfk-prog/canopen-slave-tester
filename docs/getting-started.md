# 快速开始

## 1. 准备目录

推荐工作区：

```text
~/share/lely-imx8p/
├── lely-core/
└── canopen-master/
```

Lely stage 至少需要：

```text
lely-core/build-imx8p/stage/usr/include
lely-core/build-imx8p/stage/usr/lib
```

## 2. 配置构建机

```sh
cd ~/share/lely-imx8p/canopen-master
cp cmake/build_config.local.cmake.example cmake/build_config.local.cmake
```

编辑本机文件，填写 Yocto 交叉工具链、sysroot、Lely stage 和目标板地址。

## 3. 生成 DCF

当 `master.yml` 或 EDS 有变化时：

```sh
python3 -m venv .venv-dcf-tools
.venv-dcf-tools/bin/python -m pip install ../lely-core/python/dcf-tools
cd config
../.venv-dcf-tools/bin/dcfgen -r -v -d generated master.yml
cd ..
```

## 4. 构建默认 MinSizeRel

普通构建不需要显式传 `CMAKE_BUILD_TYPE`，工程默认使用 `MinSizeRel`：

```sh
rm -rf build
cmake -S . -B build \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --verbose -j"$(nproc)"
```

CMake 配置输出应包含 `CANopen master build type: MinSizeRel`。首次构建还会自动生成
`build/third_party/spdlog/libspdlog.a`，Host 最终静态链接该库。

## 5. 部署运行

```sh
cmake --build build --target download
```

程序无命令行参数。它会执行当前启用流程并保持运行；按 `Ctrl+C` 后执行最终 Reset Communication 并退出。

## 6. 启动远程调试

`debug-deploy` 仍要求显式 `Debug` 构建。切换到调试配置后再部署：

```sh
rm -rf build
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --verbose -j"$(nproc)"
cmake --build build --target debug-deploy
```

Debug 最终参数应包含 `-O0 -g3`。`-g0` 会关闭调试信息。兼容目标：

```sh
cmake --build build --target debug
```
