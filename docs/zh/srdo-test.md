[English](../en/srdo-test.md)

# SRDO 协议验证

SRDO 流程用于验证当前配置的 CANopenNode Safety-Related Data Object profile。协议控制仍通过 Lely/SDO 完成；成对 SRDO 帧的 wire evidence 通过用途固定的 safety channel 获取。

## 当前默认状态

当前仓库只有 `CANOPEN_ENABLE_SRDO_PROCESS=1`，它是默认启用的唯一 Master 自动协议验证流程。启用该流程要求 MCU 固件和 Object Dictionary 与 `src/srdo_process.cpp` 使用的 SRDO 测试 profile 一致。

## 覆盖内容

当前实现覆盖正常路径和定向 fault case，包括：

- established-window baseline；
- NMT 非 Operational gate；
- valid receive pair 与 mapped OD 更新；
- transmit pair 与 timing window；
- Reset Communication、rebind 和 natural periodic TX；
- 与本次 SRDO 运行相关的 PDO/SYNC/SDO/NMT 回归；
- wrong inverse data；
- missing/incomplete pair timeout；
- short frame、wrong frame order；
- checksum mismatch；
- configuration-validity invalidation；
- invalid COB-ID pair；
- mapping count/length 非法；
- TX inversion/silence；
- final recovery。

## 安全边界

该流程是协议实现验证夹具，不等价于完整安全功能认证，也不证明硬件冗余、真实执行器 safe state、diagnostic coverage、WCET、SIL、PL 或法规符合性。

## 共享 wire channel

如果 GFC/SRDO 同时启用，二者按流程顺序共享第二条 Lely wire channel。SRDO 使用前会清理旧 wire evidence，避免前一个 safety-protocol 流程留下的帧被误判为本次 SRDO 证据。

## 详细参考

旧版完整英文记录保留在 [`../en/reference/CANopen_SRDO_Test.md`](../en/reference/CANopen_SRDO_Test.md)。
