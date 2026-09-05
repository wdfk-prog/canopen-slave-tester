[中文](../zh/ci-cd.md)

# CI/CD

The automation is split into three independent GitHub Actions workflows:

- `.github/workflows/ci.yml`: Cppcheck reporting;
- `.github/workflows/release.yml`: tag/manual TQ8MP build and GitHub Release publication;
- `.github/workflows/pages-doxygen.yml`: Doxygen generation and GitHub Pages deployment.

No GHCR/container-image publication is used.

## Self-hosted TQ8MP build runner

The TQ8MP release build uses a self-hosted runner because the project-specific Yocto SDK and target Lely stage are local build dependencies that are not present on GitHub-hosted runners.

Register a Linux x86-64 self-hosted runner for this repository and add the custom label:

```text
tq8mp-yocto
```

The runner must be able to read the real build environment. For the current SDK layout this means:

```text
<SDK_ROOT>/sysroots/x86_64-pokysdk-linux/usr/bin/aarch64-poky-linux/
<SDK_ROOT>/sysroots/armv8a-poky-linux/
<LELY_STAGE_ROOT>/include/
<LELY_STAGE_ROOT>/lib/pkgconfig/liblely-coapp.pc
```

Configure these non-secret repository variables under **Settings -> Secrets and variables -> Actions -> Variables**:

| Variable | Meaning | Current environment example |
| --- | --- | --- |
| `TQ8MP_YOCTO_SDK_ROOT` | Root directory of the installed TQ8MP Yocto SDK | `/opt/fsl-imx-xwayland/6.1-mickledore` |
| `TQ8MP_LELY_STAGE_ROOT` | Target-architecture Lely install/stage prefix | `/home/<user>/share/lely-imx8p/lely-core/build-imx8p/stage/usr` |

The release workflow derives the exact compiler and sysroot paths from those roots. It fails closed when the SDK compiler, target sysroot, or Lely metadata is missing.

## CI: `.github/workflows/ci.yml`

CI is triggered on push, pull requests targeting `master`/`main`, and manual dispatch. The workflow now contains only the GitHub-hosted Cppcheck reporting job; TQ8MP cross-compilation remains a release-only path.

### Cppcheck report

Cppcheck runs on a GitHub-hosted Ubuntu runner and analyzes only project-owned `src/` and `include/` code.

- warning/style/performance/portability checks are enabled;
- inconclusive findings are included;
- missing external/system include noise is suppressed;
- findings are written to the job log and `cppcheck.txt` artifact;
- findings do not fail CI, while tool/workflow execution failures still fail the job.

## GitHub Release CD: `.github/workflows/release.yml`

CD runs when a `v*` tag is pushed or when manually dispatched with a release tag.

The build job uses the same real TQ8MP self-hosted runner and produces:

```text
canopen-slave-tester-<tag>-tq8mp-aarch64.tar.gz
canopen-slave-tester-<tag>-tq8mp-aarch64.tar.gz.sha256
```

The archive contains the CMake install tree plus the target Lely shared libraries from the configured stage:

```text
bin/canopen_master
config/...
lib/liblely-*.so*
BUILD_INFO.txt
```

`BUILD_INFO.txt` records the release tag, commit, target, cross-compiler, sysroot, Lely version, and build type.

If the target does not already have matching Lely libraries installed system-wide, either install the packaged `lib/liblely-*.so*` files into the target runtime library path or launch the executable with an explicit library path, for example `cd bin && LD_LIBRARY_PATH=../lib ./canopen_master` from the extracted release directory.

A separate GitHub-hosted publication job downloads the packaged artifact and creates or updates the GitHub Release with `gh release`. The workflow uses `contents: write`; it does not use `packages: write` and does not publish to GHCR.

Typical release:

```sh
git tag v0.5.0
git push origin v0.5.0
```

After the workflow succeeds, open **Releases** on the repository page. The `.tar.gz` and `.sha256` files are attached to that release. After downloading both files into the same directory, verify the package with `sha256sum -c canopen-slave-tester-<tag>-tq8mp-aarch64.tar.gz.sha256`.

## Doxygen Pages: `.github/workflows/pages-doxygen.yml`

Pushes to `master` that touch Doxygen, source, headers, or documentation rebuild the API site. The workflow:

1. installs Doxygen and Graphviz;
2. generates `build/doxygen/html` from `Doxyfile`;
3. uploads the official GitHub Pages artifact;
4. deploys through the `github-pages` environment.

Doxygen warnings are visible but are not treated as errors. A generation/deployment failure still fails the workflow.

For the first deployment, configure **Settings -> Pages -> Build and deployment -> Source -> GitHub Actions**.

The expected site URL is:

```text
https://wdfk-prog.github.io/canopen-slave-tester/
```

## Release and validation boundary

Before publishing a production release, keep target-board/HIL acceptance separate from CI and release packaging. The CI workflow reports static Cppcheck results only. A successful TQ8MP release build establishes compiler/linker compatibility for the configured SDK, not CAN bus health, runtime permissions, target filesystem compatibility, physical wiring, protocol timing, or DUT behavior.
