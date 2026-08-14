# 完整 Debug 构建与远程调试

## 1. 构建 Lely Debug 产物

在与工程匹配的 Yocto SDK 环境中构建 Lely。推荐调试参数：

```sh
DEBUG_FLAGS="-O0 -g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls -UNDEBUG -U_FORTIFY_SOURCE"

CFLAGS="${CFLAGS:-} ${DEBUG_FLAGS}" \
CXXFLAGS="${CXXFLAGS:-} ${DEBUG_FLAGS}" \
../configure \
    --host=aarch64-poky-linux \
    --prefix=/usr \
    --disable-python \
    --disable-cython \
    --disable-tests \
    --disable-unit-tests

make -j"$(nproc)" V=1
make install DESTDIR="${PWD}/stage"
```

不要使用 `-g0`，否则主程序或动态库无法提供源码行信息。

## 2. 同步 Lely 动态库

工程根目录脚本默认先做只读检查：

```sh
./deploy_lely_libraries.sh
```

确认输出后执行：

```sh
./deploy_lely_libraries.sh --apply
```

脚本负责同步 Lely 动态库到本地 GDB sysroot 和目标板，并检查架构、符号、软链接和哈希。具体本机路径由脚本及 `deploy/local.conf` 控制。

## 3. 配置主程序

```sh
cp cmake/build_config.local.cmake.example cmake/build_config.local.cmake
```

填写：

- `CANOPEN_TOOLCHAIN_BIN_DIR`
- `CANOPEN_TOOLCHAIN_PREFIX`
- `CANOPEN_SYSROOT`
- `CANOPEN_LELY_INCLUDE_DIR`
- `CANOPEN_LELY_LIBRARY_DIR`
- `CACHED_IP_ADDR`

CAN 接口和 Node-ID 修改 `include/canopen_config.h`。

## 4. 构建主程序

工程普通构建默认是 `MinSizeRel`；本节远程调试流程必须显式指定 `Debug`。

```sh
rm -rf build
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --verbose -j"$(nproc)"
```

检查：

```sh
readelf --debug-dump=decodedline build/canopen_master | grep -m 10 'main.cpp'
```

## 5. 部署并启动 gdbserver

```sh
cmake --build build --target debug-deploy
```

默认端口为 `9091`。可通过 `CANOPEN_GDB_PORT` 修改。

## 6. GDB 验证

连接后至少检查：

```gdb
info line main
info sharedlibrary
break main
break nmtHeartbeatProcess
break sdoProcess
break finalResetProcess
```

主程序和 Lely 动态库必须加载与目标板相同 Build ID/哈希的符号文件。只有断点能够绑定并命中，才能确认源码级调试链路成立。
