# CANopen Slave Tester Documentation Portal {#mainpage}

This site combines the generated C/C++ API reference with the maintained bilingual project documentation.

## English documentation

- [Documentation index](en/index.md)
- [Getting started](en/getting-started.md)
- [Architecture](en/architecture.md)
- [Configuration](en/configuration.md)
- [Testing and validation](en/testing.md)
- [CI/CD](en/ci-cd.md)

## 中文文档

- [文档索引](zh/index.md)
- [快速开始](zh/getting-started.md)
- [架构](zh/architecture.md)
- [配置](zh/configuration.md)
- [测试与验证](zh/testing.md)
- [CI/CD](zh/ci-cd.md)

## API entry points

Use the generated Doxygen navigation for **Files**, **Globals**, and **Data Structures / Classes**.

## Source layout

- Public project headers are under `include/`.
- Runtime and protocol validation implementations are under `src/`.
- Vendored third-party code under `third_party/` is intentionally excluded from the generated API site.
