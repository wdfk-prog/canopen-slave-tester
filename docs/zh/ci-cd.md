[English](../en/ci-cd.md)

# CI/CD

自动化现在拆为三个独立的 GitHub Actions workflow：

- `.github/workflows/ci.yml`：Cppcheck 报告；
- `.github/workflows/release.yml`：tag/手动触发 TQ8MP 构建并发布 GitHub Release；
- `.github/workflows/pages-doxygen.yml`：生成 Doxygen 并部署 GitHub Pages。

不再使用 GHCR，也不再发布 Docker/OCI 镜像。

## Self-hosted TQ8MP 构建 Runner

TQ8MP Release 构建 job 使用 self-hosted runner。原因是项目实际使用的 Yocto SDK 和目标架构 Lely stage 属于本机构建依赖，GitHub Hosted Runner 默认没有这些文件。

在仓库中注册一个 Linux x86-64 self-hosted runner，并增加自定义标签：

```text
tq8mp-yocto
```

该 runner 必须能直接访问真实构建环境。按当前 SDK 布局至少应存在：

```text
<SDK_ROOT>/sysroots/x86_64-pokysdk-linux/usr/bin/aarch64-poky-linux/
<SDK_ROOT>/sysroots/armv8a-poky-linux/
<LELY_STAGE_ROOT>/include/
<LELY_STAGE_ROOT>/lib/pkgconfig/liblely-coapp.pc
```

然后进入：

```text
GitHub 仓库
-> Settings
-> Secrets and variables
-> Actions
-> Variables
```

配置两个非敏感 Repository Variables：

| 变量 | 作用 | 当前环境示例 |
| --- | --- | --- |
| `TQ8MP_YOCTO_SDK_ROOT` | 已安装的 TQ8MP Yocto SDK 根目录 | `/opt/fsl-imx-xwayland/6.1-mickledore` |
| `TQ8MP_LELY_STAGE_ROOT` | 面向目标架构构建的 Lely stage/install 前缀 | `/home/<user>/share/lely-imx8p/lely-core/build-imx8p/stage/usr` |

Release workflow 会根据这两个根目录推导真实 compiler、sysroot、Lely include/lib。任何路径不存在都会直接失败，不会退回 native 编译冒充 TQ8MP 构建。

## CI：`.github/workflows/ci.yml`

CI 在 push、目标为 `master`/`main` 的 Pull Request，以及手动触发时运行。当前 `ci.yml` 只包含 GitHub Hosted Runner 上的 Cppcheck 报告 job；TQ8MP 交叉编译仅保留在 Release workflow。

### Cppcheck 报告

Cppcheck 使用 GitHub Hosted Ubuntu Runner，只分析项目自身的 `src/` 和 `include/`：

- 开启 warning/style/performance/portability；
- 开启 inconclusive；
- 屏蔽外部/系统头文件缺失噪声；
- finding 输出到 job 日志和 `cppcheck.txt` artifact；
- finding 不导致 CI 失败，但 Cppcheck 安装、命令执行、artifact 上传等真正的 workflow 错误仍会失败。

## GitHub Release CD：`.github/workflows/release.yml`

CD 在推送 `v*` tag 时触发，也支持手动输入 release tag。

Build job 使用同一套真实 TQ8MP self-hosted runner，最终生成：

```text
canopen-slave-tester-<tag>-tq8mp-aarch64.tar.gz
canopen-slave-tester-<tag>-tq8mp-aarch64.tar.gz.sha256
```

压缩包包含 CMake install 结果以及 runner 中目标架构 Lely stage 的共享库：

```text
bin/canopen_master
config/...
lib/liblely-*.so*
BUILD_INFO.txt
```

`BUILD_INFO.txt` 会记录 tag、commit、目标平台、cross compiler、sysroot、Lely version 和 Build Type。

如果目标板没有在系统库目录中安装相同版本的 Lely，可以把 Release 包中的 `lib/liblely-*.so*` 安装到目标运行库路径；也可以在解压目录中临时使用 `cd bin && LD_LIBRARY_PATH=../lib ./canopen_master`。

随后独立的 GitHub Hosted publication job 下载该 artifact，并使用 `gh release` 创建或更新 GitHub Release。Workflow 只需要 `contents: write`，不再需要 `packages: write`，也不会向 GHCR 发布任何内容。

典型发布方式：

```sh
git tag v0.5.0
git push origin v0.5.0
```

Workflow 成功后，进入仓库 **Releases** 页面即可下载 `.tar.gz` 和 `.sha256`。将两个文件放在同一目录后，可执行 `sha256sum -c canopen-slave-tester-<tag>-tq8mp-aarch64.tar.gz.sha256` 校验发布包。

## Doxygen Pages：`.github/workflows/pages-doxygen.yml`

当 `master` 上的 Doxyfile、源码、头文件或文档发生变化时，Workflow 会：

1. 安装 Doxygen 与 Graphviz；
2. 根据 `Doxyfile` 生成 `build/doxygen/html`；
3. 上传官方 GitHub Pages artifact；
4. 通过 `github-pages` environment 部署。

Doxygen warning 只显示，不作为失败条件；真正的生成或部署失败仍会让 workflow 失败。

第一次使用需要进入：

```text
Settings
-> Pages
-> Build and deployment
-> Source
-> GitHub Actions
```

预计文档地址：

```text
https://wdfk-prog.github.io/canopen-slave-tester/
```

## Release 与验证边界

正式发布前仍应把目标板/HIL 验收与 CI、Release 构建分开看待。CI 只提供 Cppcheck 静态结果；成功的 TQ8MP Release 构建只能证明编译/链接兼容性，不能证明 CAN 总线健康、目标端权限/文件系统、物理接线、协议实时性或 DUT 行为全部通过。
