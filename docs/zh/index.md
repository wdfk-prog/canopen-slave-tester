[English](../en/index.md)

# 项目文档

本目录是 CANopen Slave Tester 维护中的中文文档。英文对应页面位于 `docs/en/`，核心主题使用相同文件名，便于中英文切换。

## 建议阅读顺序

- [快速开始](getting-started.md)：准备构建环境并得到第一个可运行二进制。
- [架构设计](architecture.md)：Host 角色、Lely 运行时、流程顺序和 wire fixture 边界。
- [配置说明](configuration.md)：编译开关、Node-ID、SocketCAN、DCF/EDS 和构建环境边界。
- [测试与验证](testing.md)：每个协议流程实际验证什么，以及哪些结论仍必须由目标板/HIL 提供。
- [CI/CD](ci-cd.md)：Cppcheck CI、TQ8MP Release 构建、GitHub Release 与 Doxygen Pages。
- [部署](deployment.md)：目标板目录与部署脚本。
- [故障排查](troubleshooting.md)：构建、CAN、DCF、self-hosted runner、Release 和 CI 常见问题。

## 协议专项

- [NMT Master 验证](nmt-master-test.md)
- [EMCY Consumer 验证](emcy-consumer-test.md)
- [GFC 协议验证](gfc-test.md)
- [SRDO 协议验证](srdo-test.md)
- [CiA 303-3 LED 手工验证](cia303-led-test.md)

## 工具与接口

- [API 与模块映射](api.md)
- [dcfgen 配置](dcfgen-setup.md)
- [VS Code/GDB 调试](vscode-debugging.md)

维护中的中英文页面是当前项目文档事实源。旧版文档中的长篇测试记录和实施历史保留在 `docs/zh/reference/` 与 `docs/en/reference/`，这些 reference 文件不保证逐行双语对应。
