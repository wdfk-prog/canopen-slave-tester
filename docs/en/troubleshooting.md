[中文](../zh/troubleshooting.md)

# Troubleshooting

## CMake reports a missing local build configuration

The default build is a TQ8MP cross build. Create the local file first:

```sh
cp cmake/build_config.local.cmake.example cmake/build_config.local.cmake
```

Then correct the Yocto toolchain, sysroot, and Lely paths.

If you intentionally want a native Linux build, use `-DCANOPEN_NATIVE_BUILD=ON` and provide native Lely include/library paths instead.

## Lely include/library directory does not exist

Confirm the selected Lely installation contains both headers and shared libraries. The library directory must also contain `pkgconfig/liblely-coapp.pc`, which CMake uses to read the installed Lely version.

A target cross build must use the target-architecture Lely stage configured for the TQ8MP environment.

## Required Lely shared library not found

CMake explicitly checks for:

- `liblely-coapp.so`
- `liblely-io2.so`
- `liblely-ev.so`
- `liblely-co.so`
- `liblely-can.so`
- `liblely-util.so`
- `liblely-libc.so`

If an installation only provides versioned sonames without the unversioned development symlink, use a development/staging install suitable for linking.

## CAN bitrate validation fails

The program expects `can1` at 1 Mbit/s. Check the host before running:

```sh
ip -details link show can1
```

The application validates the bitrate but does not configure the interface.

## Self-hosted TQ8MP build job does not start

Check that a repository self-hosted runner is online and has all required labels:

```text
self-hosted
linux
x64
tq8mp-yocto
```

Also verify `TQ8MP_YOCTO_SDK_ROOT` and `TQ8MP_LELY_STAGE_ROOT` under repository Actions Variables. The workflow intentionally does not fall back to a native build when the target SDK is unavailable.

## Boot/SDO/PDO validation times out

Check in this order:

1. Node-ID and bitrate;
2. MCU NMT state;
3. Heartbeat production and consumption;
4. DCF/EDS consistency with the MCU firmware;
5. the specific diagnostic OD required by the enabled process;
6. independent CAN trace for missing request/response frames.

Do not increase timeouts before confirming whether the expected protocol event is actually present.

## Generated DCF is stale

Regenerate from `config/master.yml` and the source EDS using the project dcfgen procedure, then review the generated master DCF and concise DCF before deployment. See [dcfgen setup](dcfgen-setup.md).

## Cppcheck reports findings in CI

Download the `cppcheck-report` artifact or inspect the job log. Cppcheck findings are report-only and do not fail CI. Missing external/system includes are suppressed, so new findings should still be investigated rather than hidden with broad suppressions. If the Cppcheck job itself is red, check for tool installation, invocation, or artifact-upload failures instead of assuming a reported finding caused the failure.

## GitHub Release CD fails

Check in this order:

1. the `tq8mp-yocto` self-hosted runner is online;
2. `TQ8MP_YOCTO_SDK_ROOT` points to the installed Yocto SDK root;
3. `TQ8MP_LELY_STAGE_ROOT` contains target headers, libraries, and `pkgconfig/liblely-coapp.pc`;
4. the Yocto compiler and `aarch64-poky-linux-readelf` exist under the expected SDK layout;
5. the workflow has `contents: write`;
6. the release tag starts with `v`;
7. the cross-build artifact is confirmed as `Machine: AArch64`.

If the build job succeeds but the Release job fails, inspect the `publish-release` job separately; build evidence and GitHub Release publication are distinct stages.
