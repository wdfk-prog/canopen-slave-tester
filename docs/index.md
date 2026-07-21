# CANopen Slave Tester 文档

本目录覆盖从 dcfgen 环境部署、首次构建和目标板运行，到 P0/P1 实测证据、运行时设计和公共 API。首次接触工程时按“首次使用”顺序阅读；定位问题时直接进入配置、结果证据或故障排查文档。

## 首次使用

1. [dcfgen 安装与使用](dcfgen-setup.md)：创建 `.venv-dcf-tools`、安装 dcf-tools、生成并部署 `master.dcf`。
2. [入门教程](getting-started.md)：配置网络、交叉编译、部署和完成首次 P1 运行。
3. [配置手册](configuration.md)：理解 `tester.conf`、`master.yml` 和 P1 推荐配置。
4. [构建与部署](deployment.md)：安装目标板 Lely 运行库、使用 CMake 部署目标和远程调试。
5. [P0/P1 验收结果与证据](acceptance.md)：查看当前版本的实测结论、原始证据和未闭环项。

## 开发和维护

- [设计文档](design.md)：系统边界、组件、生命周期、线程、资源和错误模型。
- [API 文档](api.md)：命令行接口、退出码和 C++ 公共接口。

## 任务导航

| 任务                                | 文档入口                                                                          |
| ----------------------------------- | --------------------------------------------------------------------------------- |
| 第一次在 Ubuntu 上构建              | [入门教程：准备构建主机](getting-started.md#准备构建主机)                         |
| 安装或修复 `.venv-dcf-tools`        | [dcfgen 安装与使用](dcfgen-setup.md)                                              |
| 避免 125 帧批量 Reset Communication | [配置手册：使用单帧广播复位](configuration.md#使用单帧广播复位)                   |
| 修改 CAN 接口、Node-ID 或超时       | [配置手册：tester.conf](configuration.md#配置-testerconf)                         |
| 部署到 TQ8MP                        | [构建与部署](deployment.md)                                                       |
| 查看 P0/P1 当前结论和证据           | [P0/P1 验收结果与证据](acceptance.md)                                             |
| 了解 `LelyRuntime` 资源关系         | [设计文档：运行时资源图](design.md#运行时资源图)                                  |
| 查找 CLI 和 C++ 接口                | [API 文档](api.md)                                                                |


## 文档维护规则

- 先写可执行步骤，再说明原因。
- 命令必须从明确的工作目录开始，并使用工程中的真实文件名。
- 配置字段应同时说明单位、范围、默认值和副作用。
- 验收结果文档只记录已经执行的结论、原始证据和缺口；操作步骤放在教程、配置或部署文档中。
- 修改 `master.yml` 后必须重新运行 `dcfgen`，并检查 `generated/master.dcf`。
- 修改运行时公共接口后同步更新 [API 文档](api.md)；修改生命周期或线程模型后同步更新 [设计文档](design.md)。
