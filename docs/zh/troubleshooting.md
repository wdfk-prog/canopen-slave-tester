[English](../en/troubleshooting.md)

# 故障排查

## CMake 提示缺少本机 build config

默认构建就是 TQ8MP 交叉编译，因此先创建：

```sh
cp cmake/build_config.local.cmake.example cmake/build_config.local.cmake
```

然后填写正确的 Yocto toolchain、sysroot 和 Lely 路径。

如果本来就要做 native Linux 编译，则使用 `-DCANOPEN_NATIVE_BUILD=ON`，并显式提供 native Lely include/library 路径。

## Lely include/library 目录不存在

确认当前 Lely 安装同时包含头文件和 shared libraries。library 目录还必须有 `pkgconfig/liblely-coapp.pc`，CMake 会从其中读取 Lely 版本。

目标交叉构建必须链接 TQ8MP 环境配置的目标架构 Lely stage。

## CMake 找不到 Lely shared library

项目会显式检查：

- `liblely-coapp.so`
- `liblely-io2.so`
- `liblely-ev.so`
- `liblely-co.so`
- `liblely-can.so`
- `liblely-util.so`
- `liblely-libc.so`

如果安装目录只有带版本号的 soname、没有用于开发链接的无版本 symlink，应改用完整 development/stage 安装。

## CAN bitrate 校验失败

程序要求 `can1` 为 1 Mbit/s。运行前检查：

```sh
ip -details link show can1
```

程序只校验，不负责配置接口。

## Self-hosted TQ8MP 编译 job 不启动

确认仓库 self-hosted runner 在线，并且具有完整标签：

```text
self-hosted
linux
x64
tq8mp-yocto
```

同时检查仓库 Actions Variables 中的 `TQ8MP_YOCTO_SDK_ROOT` 与 `TQ8MP_LELY_STAGE_ROOT`。Workflow 在目标 SDK 不存在时会直接失败，不会退回 native 编译。

## Boot/SDO/PDO 等流程超时

建议按以下顺序确认：

1. Node-ID 与 bitrate；
2. MCU NMT state；
3. Heartbeat producer/consumer；
4. DCF/EDS 与 MCU 固件是否一致；
5. 当前流程要求的 diagnostic OD 是否存在；
6. 独立 CAN trace 中请求/响应是否真实出现。

没有先确认协议事件是否存在前，不要仅靠放大 timeout 掩盖问题。

## 生成 DCF 已过期

按项目 dcfgen 流程使用 `config/master.yml` 和源 EDS 重新生成，部署前复核 master DCF 和 concise DCF。详见 [dcfgen 配置](dcfgen-setup.md)。

## CI 中出现 Cppcheck finding

查看 job 日志或下载 `cppcheck-report` artifact。Cppcheck finding 仅用于报告，不会导致 CI 失败；外部/系统 include 缺失已经屏蔽，因此新增 finding 仍应定位原因，而不是用大范围 suppression 隐藏。如果 Cppcheck job 本身变红，应优先检查工具安装、命令执行或 artifact 上传失败，而不是默认认为 finding 导致失败。

## GitHub Release CD 失败

建议按以下顺序检查：

1. `tq8mp-yocto` self-hosted runner 是否在线；
2. `TQ8MP_YOCTO_SDK_ROOT` 是否指向真实 Yocto SDK 根目录；
3. `TQ8MP_LELY_STAGE_ROOT` 是否包含目标头文件、共享库和 `pkgconfig/liblely-coapp.pc`；
4. SDK 布局下是否存在 compiler 和 `aarch64-poky-linux-readelf`；
5. workflow 是否具有 `contents: write`；
6. release tag 是否以 `v` 开头；
7. 交叉编译结果是否通过 `Machine: AArch64` 检查。

如果 build job 已成功但 Release 页面没有附件，应单独检查 `publish-release` job；交叉编译成功与 GitHub Release 发布成功是两个独立阶段。
