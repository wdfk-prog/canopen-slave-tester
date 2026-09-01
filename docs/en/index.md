[中文](../zh/index.md)

# Documentation

This directory contains the maintained English documentation for CANopen Slave Tester. The Chinese counterparts are under `docs/zh/` with the same topic names.

## Start here

- [Getting started](getting-started.md): prepare the build environment and reach the first runnable binary.
- [Architecture](architecture.md): roles, Lely runtime, process ordering, and wire-fixture boundaries.
- [Configuration](configuration.md): compile-time switches, node IDs, SocketCAN settings, DCF/EDS inputs, and build-environment boundaries.
- [Testing and validation](testing.md): what each protocol process validates and what still requires target/HIL evidence.
- [CI/CD](ci-cd.md): Cppcheck, real TQ8MP cross-builds, GitHub Releases, and Doxygen Pages.
- [Deployment](deployment.md): target-board layout and deploy helpers.
- [Troubleshooting](troubleshooting.md): build, CAN, DCF, self-hosted runner, Release, and CI failures.

## Protocol-specific guides

- [NMT master validation](nmt-master-test.md)
- [EMCY consumer validation](emcy-consumer-test.md)
- [GFC protocol validation](gfc-test.md)
- [SRDO protocol validation](srdo-test.md)
- [CiA 303-3 LED manual validation](cia303-led-test.md)

## Tooling/reference guides

- [API and module map](api.md)
- [dcfgen setup](dcfgen-setup.md)
- [VS Code/GDB debugging](vscode-debugging.md)

The maintained bilingual pages are the canonical project documentation. Historical and very detailed notes from the earlier documentation layout are retained under `docs/zh/reference/` and `docs/en/reference/`; those reference files are not guaranteed to be line-for-line bilingual counterparts.
