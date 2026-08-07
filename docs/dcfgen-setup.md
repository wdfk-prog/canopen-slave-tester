# 安装和使用 dcfgen

`dcfgen` 在构建主机运行，用于根据 `config/master.yml` 和从机 EDS 生成 Lely 主站加载的 `master.dcf`。目标板只需要生成结果。

## 安装

优先使用与运行时相同 Lely 源码中的 `python/dcf-tools`：

```sh
cd ~/share/lely-imx8p/canopen-master
python3 -m venv .venv-dcf-tools
.venv-dcf-tools/bin/python -m pip install ../lely-core/python/dcf-tools
```

## 生成

```sh
cd config
../.venv-dcf-tools/bin/dcfgen -r -v -d generated master.yml
cd ..
```

预期输出至少包括：

```text
config/generated/master.dcf
config/generated/master.bin
config/generated/project.dcfgen.eds
```

## 校验

生成后确认：

- 主站 Node-ID 与 `CANOPEN_MASTER_NODE_ID` 一致；
- Node-ID 1 的从机配置引用正确 EDS；
- SDO/PDO COB-ID 与目标固件一致；
- 部署前 `master.dcf` 已更新，不是旧时间戳产物。

修改 `master.yml` 或 EDS 后必须重新生成并重新部署。
