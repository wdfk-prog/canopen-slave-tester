# CANopen Master 文档

本文档集描述当前 `canopen_master 0.4.0` 基础工程。当前代码直接使用 Lely `AsyncMaster` 和 `BasicDriver`，不包含应用框架、运行时配置解析或通用自动化测试框架。

## 导航

- [快速开始](getting-started.md)：准备本地配置、构建、部署和首次运行。
- [编译期配置](configuration.md)：`include/canopen_config.h` 与 `config/master.yml`。
- [架构设计](design.md)：双线程模型、资源生命周期和 callback 边界。
- [NMT Master 测试](CANopen_NMT_Master_Test.md)：Master/Slave 编译角色、Lely BasicSlave peer 和 NMT 命令验证流程。
- [B06 EMCY Consumer 测试](CANopen_EMCY_Consumer_Test.md)：Host EMCY producer、MCU `0x2301` diagnostic 与 Reset Communication callback 重绑。
- [J03/B09G GFC 测试](CANopen_GFC_Test.md)：独立 Lely wire fixture、MCU `0x1300/0x2302` 与 GFC consumer/producer。
- [重构实施计划](CANopen_Master_Refactor_Plan.md)：当前方案、阶段、验收和风险。
- [代码接口](api.md)：当前 Process、callback 和退出行为。
- [构建与部署](deployment.md)：CMake target、目标板目录和脚本变量。
- [完整 Debug 流程](debug-build-and-deploy.md)：Lely、主程序、gdbserver 和符号验证。
- [dcfgen](dcfgen-setup.md)：生成 `master.dcf`。
- [VS Code 调试](vscode-debugging.md)：工作区和 `launch.json` 配置。
- [故障排查](troubleshooting.md)：构建、运行、CAN 和 GDB 常见问题。
- [验证状态](acceptance.md)：本次实际完成与未完成的验证边界。
