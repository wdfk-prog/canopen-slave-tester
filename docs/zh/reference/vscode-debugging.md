# VS Code 远程调试

## 工作区

推荐在包含两个相邻仓库的目录打开 VS Code：

```text
~/share/lely-imx8p/
├── .vscode/
├── lely-core/
└── canopen-master/
```

```sh
code ~/share/lely-imx8p
```

## 示例文件

将 `docs/examples/.vscode/` 中的文件复制或合并到工作区 `.vscode/`。重点修改：

- `program`：`${workspaceFolder}/canopen-master/build/canopen_master`
- `cwd`：`${workspaceFolder}/canopen-master`
- `miDebuggerServerAddress`：目标板 IP 与 gdbserver 端口
- `miDebuggerPath`：Yocto 交叉 GDB
- `sourceFileMap`：本机构建路径与工作区路径
- sysroot 和 Lely 动态库搜索路径

不要覆盖已有工作区配置；按本机目录逐项合并。

## 启动

先执行：

```sh
cmake --build canopen-master/build --target debug-deploy
```

再从 VS Code 选择 `CANopen Master - Remote GDB Debug`。

## 验证

在 Debug Console 或 GDB 中确认：

```gdb
info line main
break main
break nmtHeartbeatProcess
break sdoProcess
break finalResetProcess
info sharedlibrary
```

断点灰色、无法绑定时，优先检查本地 ELF 与目标板 ELF 的 Build ID，以及 `sourceFileMap` 是否匹配编译时绝对路径。
