[中文](../zh/deployment.md)

# Deployment

## Local deployment settings

Create a local configuration from the example:

```sh
cp deploy/local.conf.example deploy/local.conf
```

`deploy/local.conf` may contain target address, SSH user/port, and optional password authentication. It is listed in `.gitignore`; an already-tracked copy must be removed from the Git index separately. Prefer SSH keys when practical and protect any local password file with restrictive permissions.

## CMake deployment target

The deployment helpers are available in the default TQ8MP cross-build mode. Release builds use this target build contract on the self-hosted TQ8MP runner; CI is Cppcheck-only.

After a successful target build:

```sh
cmake --build build --target download
```

For a Debug build and gdbserver deployment:

```sh
cmake --build build-debug --target debug-deploy
```

The debug target checks that the active build configuration is actually `Debug` before launching the remote debug path.

## Target layout

The deployment scripts use the project target directory and place the executable separately from runtime configuration. The application resolves `CANOPEN_MASTER_DCF_PATH` relative to its executable working directory, so keep the installed `bin/` and `config/` relationship intact.

The installed CMake payload contains:

```text
bin/canopen_master
config/project.eds
config/master.yml
config/master.dcf
config/mcu_node_1.bin
config/generated/master.bin
config/generated/project.dcfgen.eds
```

The deploy scripts may use a target-specific layout configured in their local settings; verify the actual remote path before overwriting an existing installation.

## Lely runtime libraries

The target executable links against the staged Lely shared libraries used by the cross build. `deploy/install_lely.sh` and `deploy_lely_libraries.sh` are provided to support target-side Lely installation workflows.

Release packages copy the same target-architecture Lely shared libraries referenced by the configured TQ8MP build runner. Keep the runner Lely stage synchronized with the target deployment environment. When running directly from an extracted Release package without system-wide Lely installation, enter the package `bin/` directory and use `LD_LIBRARY_PATH=../lib ./canopen_master`.

## Pre-run checks

Before starting the target application:

1. verify the physical CAN bus and termination;
2. configure `can1` for 1 Mbit/s and set it UP;
3. verify the MCU firmware matches the selected validation profile;
4. confirm generated DCF/EDS files match the MCU Object Dictionary;
5. capture a baseline `candump` when protocol timing/wire evidence is part of acceptance.
