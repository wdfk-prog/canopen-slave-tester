[English](../en/dcfgen-setup.md)

# dcfgen 配置

项目配置源为 `config/master.yml` 与 `config/project.eds`，生成的 master/concise DCF 位于 `config/generated/`。

当前配置注释基于项目使用的 Lely `dcfgen` 2.4.0 工作流。

## 生成

准备好匹配版本的 `dcf-tools` 后：

```sh
cd config
<path-to-dcfgen> -r -v -d generated master.yml
```

旧环境曾使用本机 `.venv-dcf-tools`，但该虚拟环境不属于仓库交付契约。

## 部署前复核

生成后至少检查：

- Master Node-ID 与 bitrate；
- `0x1F80` NMT startup；
- `0x1F81` slave assignment/boot 策略；
- Heartbeat producer/consumer；
- Node 1 concise DCF；
- 相对 EDS default 有变化的 PDO/SYNC/EMCY 通信参数。

基线通信配置或源 EDS 改变后必须重新生成。运行期临时测试值通常应由所属流程恢复，而不是固化进配置源。

## 详细参考

旧版记录保留在 [`reference/dcfgen-setup.md`](reference/dcfgen-setup.md)。
