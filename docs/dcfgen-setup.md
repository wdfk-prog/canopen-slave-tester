# 安装和使用 dcfgen

`dcfgen` 只运行在 Ubuntu 构建主机上，用于把 `config/master.yml` 和从机 EDS/DCF 生成 Lely 主站加载的 `master.dcf`。目标板不需要安装 Python、虚拟环境或 `dcfgen`；目标板只部署生成结果。

## 选择与 Lely 版本一致的 dcf-tools

当前工程运行时基于 Lely `2.4.0`。优先从同一份 Lely `2.4.0` 源码中的 `python/dcf-tools` 安装，避免生成器与运行库的对象字段或启动策略出现版本差异。

若构建主机只有已安装好的 `.venv-dcf-tools`，先执行“验证现有环境”，不需要重复安装。

## 在工程目录创建虚拟环境

安装 Ubuntu 基础依赖：

```sh
sudo apt-get update
sudo apt-get install -y python3 python3-venv python3-pip
```

进入工程根目录并创建独立虚拟环境：

```sh
cd ~/share/lely-imx8p/canopen-slave-tester
python3 -m venv .venv-dcf-tools
```

升级虚拟环境内的 Python 打包工具：

```sh
.venv-dcf-tools/bin/python -m pip install --upgrade pip setuptools wheel
```

`.venv-dcf-tools/` 是本机构建工具，不应提交到版本库，也不应复制到 TQ8MP。

## 从 Lely 2.4.0 源码安装

假设 Lely 源码位于 `/path/to/lely-core`，并且存在：

```text
/path/to/lely-core/python/dcf-tools/setup.py
```

安装到工程虚拟环境：

```sh
cd ~/share/lely-imx8p/canopen-slave-tester
.venv-dcf-tools/bin/python -m pip install /path/to/lely-core/python/dcf-tools
```

安装完成后，`dcfgen`、`dcfchk` 和相关 Python 模块都位于 `.venv-dcf-tools` 内，不污染系统 Python。

### 从本地 wheel 安装

构建主机无法联网时，可在保存 Lely 2.4.0 源码的机器上生成 wheel：

```sh
cd /path/to/lely-core/python/dcf-tools
python3 -m venv .venv-wheel
.venv-wheel/bin/python -m pip install --upgrade pip setuptools wheel
.venv-wheel/bin/python setup.py bdist_wheel
```

将 `dist/dcf_tools-*.whl` 复制到工程主机，然后安装：

```sh
cd ~/share/lely-imx8p/canopen-slave-tester
.venv-dcf-tools/bin/python -m pip install /path/to/dcf_tools-2.4.0-py3-none-any.whl
```

wheel 文件名以实际生成结果为准。

## 使用 Ubuntu 软件包作为备用方案

Lely 官方 Ubuntu PPA 提供 `python3-dcf-tools`。该方式安装到系统 Python，版本由 PPA 当前软件包决定，不保证与项目的 Lely `2.4.0` 完全一致，因此只作为临时或对照环境使用。

```sh
sudo add-apt-repository ppa:lely/ppa
sudo apt-get update
sudo apt-get install -y python3-dcf-tools
```

项目生成正式 `master.dcf` 时，仍优先使用 `.venv-dcf-tools/bin/dcfgen`，避免误调用 `/usr/bin/dcfgen`。

## 验证现有环境

在工程根目录运行：

```sh
cd ~/share/lely-imx8p/canopen-slave-tester
source .venv-dcf-tools/bin/activate
which python
which dcfgen
python -m pip show dcf-tools
dcfgen --help
dcfchk --help
```

路径应指向当前工程：

```text
.../canopen-slave-tester/.venv-dcf-tools/bin/python
.../canopen-slave-tester/.venv-dcf-tools/bin/dcfgen
```

完成检查后退出虚拟环境：

```sh
deactivate
```

## 生成项目 master.dcf

确认从机 EDS 输入存在：

```sh
cd ~/share/lely-imx8p/canopen-slave-tester
test -s config/generated/project.dcfgen.eds
```

使用虚拟环境生成：

```sh
source .venv-dcf-tools/bin/activate
cd config
dcfgen -r -v -d generated master.yml
cd ..
deactivate
```

也可以不激活环境，直接使用完整路径：

```sh
cd ~/share/lely-imx8p/canopen-slave-tester/config
../.venv-dcf-tools/bin/dcfgen -r -v -d generated master.yml
cd ..
```

必须从 `config` 目录执行，因为 `master.yml` 中：

```yaml
dcf: "generated/project.dcfgen.eds"
```

按 `dcfgen` 的当前工作目录解析。

## 检查生成结果

确认输出文件非空：

```sh
test -s config/generated/master.dcf
ls -lh config/generated/master.dcf config/generated/master.bin
```

检查当前 P1 NMT 启动配置：

```sh
grep -n -A5 '^\[1F80\]' config/generated/master.dcf
grep -n -A5 '^\[1F81Value\]' config/generated/master.dcf
```

当前基线应包含：

```ini
[1F80]
DefaultValue=0x0000000D

[1F81Value]
NrOfEntries=1
1=0x00000001
```

其中 `1F81:01 = 0x00000001` 与：

```yaml
mcu_node_1:
  boot: false
  mandatory: false
  reset_communication: true
```

对应。该配置允许 Lely 在启动时发送单帧广播 `000#8200`，避免向 Node-ID 2～126 逐个发送 125 帧定向 Reset Communication。

可选地检查生成 DCF：

```sh
.venv-dcf-tools/bin/dcfchk config/generated/master.dcf
```

`dcfchk` 成功只能说明未发现其已覆盖的格式问题，不能替代目标板联调。

## 部署生成结果到 TQ8MP

重新生成后必须部署 `config/generated/master.dcf`。一键部署：

```sh
cd ~/share/lely-imx8p/canopen-slave-tester
cmake --build build --target download
```

项目部署映射为：

```text
config/generated/master.dcf
    -> /opt/Ultra/Debug/canopen-slave-tester/config/master.dcf
```

只更新 DCF 时也可以手工复制：

```sh
scp config/generated/master.dcf \
  root@TARGET_IP:/opt/Ultra/Debug/canopen-slave-tester/config/master.dcf
```

部署前后比较 SHA-256：

```sh
sha256sum config/generated/master.dcf
ssh root@TARGET_IP \
  'sha256sum /opt/Ultra/Debug/canopen-slave-tester/config/master.dcf'
```

两个校验值必须一致。目标板程序重启后才会加载新 DCF。

## 修复损坏或错误版本的虚拟环境

出现以下情况时重建环境：

- `dcfgen: command not found`；
- `ModuleNotFoundError: No module named 'dcfgen'`；
- `which dcfgen` 指向 `/usr/bin/dcfgen`；
- `pip show dcf-tools` 显示了非预期版本；
- 同一份 `master.yml` 生成结果与已确认基线不一致。

重建：

```sh
cd ~/share/lely-imx8p/canopen-slave-tester
deactivate 2>/dev/null || true
rm -rf .venv-dcf-tools
python3 -m venv .venv-dcf-tools
.venv-dcf-tools/bin/python -m pip install --upgrade pip setuptools wheel
.venv-dcf-tools/bin/python -m pip install /path/to/lely-core/python/dcf-tools
```

然后重新执行环境验证和 DCF 生成检查。

## 常见问题

### 找不到从机 EDS

典型错误来自在工程根目录直接运行：

```sh
dcfgen -r -v -d generated config/master.yml
```

此时 `generated/project.dcfgen.eds` 会按错误的当前目录解析。进入 `config` 再执行。

### 修改 master.yml 后目标板行为没有变化

依次确认：

1. 已重新运行 `dcfgen`；
2. `config/generated/master.dcf` 修改时间和校验值已变化；
3. 新文件已部署为目标板的 `config/master.dcf`；
4. 目标板程序已退出并重新启动；
5. 启动日志已从 Node-ID 2～126 的定向复位变为 `NMT: sending command specifier 130 to node 0`。

### 生成了额外的 .bin 文件

`dcfgen` 会根据 identity、软件版本、配置文件和 SDO 写入需求生成 concise DCF `.bin`。当前 P1 使用 `boot: false`，不自动向从机下发配置；后续启用 boot 配置时，需要同时确认 `.bin` 的运行时路径和部署策略。

## 官方资料

- [Lely EDS/DCF tools](https://opensource.lely.com/canopen/docs/dcf-tools/)
- [Lely installation](https://opensource.lely.com/canopen/docs/installation/)
- [Lely dcfgen source-install discussion](https://gitlab.com/lely_industries/lely-core/-/issues/101)
