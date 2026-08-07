#!/usr/bin/env bash
# Deploy one complete Lely shared-library build to the local GDB sysroot and target board.
# The script is dry-run by default. Pass --apply to perform cleanup, deployment,
# verification, and automatic rollback on failure. Rollback snapshots are temporary
# and are removed after a successful deployment. The script does not start, stop, or
# inspect the CANopen master or gdbserver processes.

set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_NAME=${0##*/}
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
WORKSPACE_DIR=$(cd -- "${SCRIPT_DIR}/.." && pwd)
DEPLOY_CONFIG=${CANOPEN_DEPLOY_CONFIG:-"${SCRIPT_DIR}/deploy/local.conf"}

if [[ -r ${DEPLOY_CONFIG} ]]; then
    # shellcheck source=/dev/null
    source "${DEPLOY_CONFIG}"
fi

STAGE_LIB="${WORKSPACE_DIR}/lely-core/build-imx8p/stage/usr/lib"
if [[ -n ${SDKTARGETSYSROOT:-} ]]; then
    SYSROOT_LIB="${SDKTARGETSYSROOT}/usr/local/lib"
else
    SYSROOT_LIB="/opt/fsl-imx-xwayland/6.1-mickledore/sysroots/armv8a-poky-linux/usr/local/lib"
fi
TARGET="${CANOPEN_TARGET_USER:-root}@${CANOPEN_TARGET_IP:-172.168.1.130}"
TARGET_PORT=${CANOPEN_TARGET_SSH_PORT:-22}
TARGET_CONNECT_TIMEOUT=${CANOPEN_SSH_CONNECT_TIMEOUT:-5}
TARGET_PASSWORD=${CANOPEN_TARGET_PASSWORD:-}
TARGET_LIB="/usr/local/lib"
TARGET_APP="/opt/Ultra/Debug/canopen-master/bin/canopen_master.elf"
APPLY=0
KEEP_WORKDIR=0

WORKDIR=""
BUNDLE=""
SOURCE_MANIFEST=""
DEPLOY_ID=""
LOCAL_BACKUP_DIR=""
REMOTE_BACKUP_DIR=""
HOST_CHANGED=0
TARGET_CHANGED=0
SNAPSHOT_CLEANUP_OK=1

usage()
{
    cat <<USAGE
Usage:
  ${SCRIPT_NAME} [options]

Default behavior is a read-only scan. Use --apply to deploy.
The script does not start, stop, or inspect the CANopen master or gdbserver processes.
Target defaults are loaded from deploy/local.conf when that file is readable.

Options:
  --apply                       Clean, replace, and verify both destinations.
                                Temporary rollback snapshots are removed on success.
  --stage-lib DIR               Source stage library directory.
                                Default: ${STAGE_LIB}
  --sysroot-lib DIR             Local GDB sysroot library directory.
                                Default: ${SYSROOT_LIB}
  --target USER@HOST            SSH target.
                                Default: ${TARGET}
  --target-port PORT            SSH port.
                                Default: ${TARGET_PORT}
  --target-lib DIR              Target-board library directory.
                                Default: ${TARGET_LIB}
  --target-app FILE             Target executable used for the final ldd check.
                                Default: ${TARGET_APP}
  --keep-workdir                Keep the temporary bundle directory for diagnostics.
  -h, --help                    Show this help.

Examples:
  ${SCRIPT_NAME}
  ${SCRIPT_NAME} --apply
  ${SCRIPT_NAME} --target root@172.168.1.130 --target-port 22 --apply
USAGE
}

log()
{
    printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"
}

warn()
{
    printf '[%s] WARNING: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >&2
}

die()
{
    printf '[%s] ERROR: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >&2
    return 1
}

require_command()
{
    command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

remote_ssh()
{
    if [[ -n ${TARGET_PASSWORD} ]]; then
        SSHPASS=${TARGET_PASSWORD} sshpass -e ssh \
            -p "${TARGET_PORT}" \
            -o "ConnectTimeout=${TARGET_CONNECT_TIMEOUT}" \
            -o ConnectionAttempts=1 "$@"
    else
        ssh -p "${TARGET_PORT}" \
            -o "ConnectTimeout=${TARGET_CONNECT_TIMEOUT}" \
            -o ConnectionAttempts=1 "$@"
    fi
}

normalize_dir()
{
    local dir=$1

    [[ -n ${dir} ]] || die "Directory path must not be empty."
    [[ ${dir} == /* ]] || die "Directory path must be absolute: ${dir}"
    [[ ${dir} != / ]] || die "Refusing to use '/' as a deployment directory."
    printf '%s\n' "${dir%/}"
}

collect_library_names()
{
    local dir=$1

    find "${dir}" -mindepth 1 -maxdepth 1 \
        \( -type f -o -type l \) -name 'liblely-*.so*' -printf '%f\0' |
        LC_ALL=C sort -z
}

generate_manifest()
{
    local dir=$1
    local output=$2
    local name target hash

    : >"${output}"
    while IFS= read -r -d '' name; do
        if [[ -L ${dir}/${name} ]]; then
            target=$(readlink "${dir}/${name}")
            printf 'L\t%s\t%s\n' "${name}" "${target}" >>"${output}"
        elif [[ -f ${dir}/${name} ]]; then
            hash=$(sha256sum "${dir}/${name}" | awk '{print $1}')
            printf 'F\t%s\t%s\n' "${name}" "${hash}" >>"${output}"
        fi
    done < <(collect_library_names "${dir}")
}

print_build_ids()
{
    local dir=$1
    local name build_id

    while IFS= read -r -d '' name; do
        [[ -f ${dir}/${name} && ! -L ${dir}/${name} ]] || continue
        build_id=$(LC_ALL=C readelf -n "${dir}/${name}" 2>/dev/null |
            awk '/Build ID:/ {print $3; exit}')
        printf '  %-42s %s\n' "${name}" "${build_id:-<none>}"
    done < <(collect_library_names "${dir}")
}

validate_source_tree()
{
    local name target resolved machine count=0
    local build_id sections

    [[ -d ${STAGE_LIB} ]] || die "Stage library directory does not exist: ${STAGE_LIB}"

    while IFS= read -r -d '' name; do
        count=$((count + 1))
        if [[ -L ${STAGE_LIB}/${name} ]]; then
            target=$(readlink "${STAGE_LIB}/${name}")
            [[ ${target} != /* ]] || die "Absolute symlink is not allowed: ${name} -> ${target}"
            resolved=$(readlink -f "${STAGE_LIB}/${name}") ||
                die "Broken source symlink: ${name} -> ${target}"
            [[ ${resolved} == "${STAGE_LIB}/"* ]] ||
                die "Source symlink escapes stage directory: ${name} -> ${target}"
            continue
        fi

        LC_ALL=C readelf -h "${STAGE_LIB}/${name}" >/dev/null 2>&1 ||
            die "Source file is not a readable ELF object: ${STAGE_LIB}/${name}"
        machine=$(LC_ALL=C readelf -h "${STAGE_LIB}/${name}" |
            awk -F: '/^[[:space:]]*Machine:/ {sub(/^[[:space:]]+/, "", $2); print $2; exit}')
        [[ ${machine} == AArch64 ]] ||
            die "Unexpected ELF machine for ${name}: ${machine:-unknown}"

        sections=$(LC_ALL=C readelf -SW "${STAGE_LIB}/${name}")
        grep -Eq '\.debug_info([[:space:]]|$)' <<<"${sections}" ||
            die "Debug library is missing .debug_info: ${STAGE_LIB}/${name}"
        grep -Eq '\.debug_line([[:space:]]|$)' <<<"${sections}" ||
            die "Debug library is missing .debug_line: ${STAGE_LIB}/${name}"

        build_id=$(LC_ALL=C readelf -n "${STAGE_LIB}/${name}" |
            awk '/Build ID:/ {print $3; exit}')
        [[ -n ${build_id} ]] ||
            die "Debug library has no Build ID: ${STAGE_LIB}/${name}"
    done < <(collect_library_names "${STAGE_LIB}")

    ((count > 0)) || die "No liblely-*.so* files found in ${STAGE_LIB}"
    [[ -e ${STAGE_LIB}/liblely-io2.so.2 ]] ||
        die "Required source library is missing: liblely-io2.so.2"
}

create_bundle()
{
    local file_list="${WORKDIR}/library-files.list"
    local payload="${WORKDIR}/bundle/payload"

    mkdir -p "${payload}"
    collect_library_names "${STAGE_LIB}" >"${file_list}"

    (
        cd "${STAGE_LIB}"
        tar --null -T "${file_list}" -cpf -
    ) | (
        cd "${payload}"
        tar -xpf -
    )

    SOURCE_MANIFEST="${WORKDIR}/bundle/source.manifest"
    generate_manifest "${payload}" "${SOURCE_MANIFEST}"

    BUNDLE="${WORKDIR}/lely-libraries-${DEPLOY_ID}.tar"
    tar -C "${WORKDIR}/bundle" -cpf "${BUNDLE}" payload source.manifest
}

scan_local_sysroot()
{
    local manifest="${WORKDIR}/sysroot-before.manifest"

    log "Scanning local GDB sysroot: ${SYSROOT_LIB}"
    if [[ ! -d ${SYSROOT_LIB} ]]; then
        printf '  Directory does not exist and will be created during deployment.\n'
        return
    fi

    generate_manifest "${SYSROOT_LIB}" "${manifest}"
    if [[ -s ${manifest} ]]; then
        sed 's/^/  /' "${manifest}"
    else
        printf '  No existing liblely-*.so* files.\n'
    fi
}

scan_target()
{
    log "Scanning target: ${TARGET}:${TARGET_LIB}"
    remote_ssh "${TARGET}" bash -s -- "${TARGET_LIB}" "${TARGET_APP}" <<'REMOTE_SCAN'
set -Eeuo pipefail
IFS=$'\n\t'

target_lib=$1
target_app=$2

collect_names()
{
    find "$1" -mindepth 1 -maxdepth 1 \
        \( -type f -o -type l \) -name 'liblely-*.so*' -printf '%f\0' |
        LC_ALL=C sort -z
}

if [[ -d ${target_lib} ]]; then
    found=0
    while IFS= read -r -d '' name; do
        found=1
        if [[ -L ${target_lib}/${name} ]]; then
            printf '  L\t%s\t%s\n' "${name}" "$(readlink "${target_lib}/${name}")"
        else
            printf '  F\t%s\t%s\n' "${name}" \
                "$(sha256sum "${target_lib}/${name}" | awk '{print $1}')"
        fi
    done < <(collect_names "${target_lib}")
    ((found == 1)) || printf '  No existing liblely-*.so* files.\n'
else
    printf '  Directory does not exist.\n'
fi

if [[ -e ${target_app} ]]; then
    printf '  ldd result for liblely-io2:\n'
    ldd "${target_app}" 2>/dev/null | grep 'liblely-io2' | sed 's/^/    /' || true
fi
REMOTE_SCAN
}

backup_local_sysroot()
{
    local archive="${LOCAL_BACKUP_DIR}/sysroot-before.tar"
    local list_file="${WORKDIR}/sysroot-before.list"

    if [[ ! -d ${SYSROOT_LIB} ]]; then
        : >"${LOCAL_BACKUP_DIR}/sysroot-before.manifest"
        return
    fi

    generate_manifest "${SYSROOT_LIB}" "${LOCAL_BACKUP_DIR}/sysroot-before.manifest"
    collect_library_names "${SYSROOT_LIB}" >"${list_file}"
    [[ -s ${list_file} ]] || return 0

    sudo tar -C "${SYSROOT_LIB}" --null -T "${list_file}" -cpf "${archive}"
    sudo chmod 0644 "${archive}"
}

clean_local_sysroot()
{
    [[ -d ${SYSROOT_LIB} ]] || return
    sudo find "${SYSROOT_LIB}" -mindepth 1 -maxdepth 1 \
        \( -type f -o -type l \) -name 'liblely-*.so*' -delete
}

restore_local_sysroot()
{
    local archive="${LOCAL_BACKUP_DIR}/sysroot-before.tar"

    warn "Rolling back local GDB sysroot."
    sudo mkdir -p "${SYSROOT_LIB}"
    clean_local_sysroot
    if [[ -f ${archive} ]]; then
        sudo tar -C "${SYSROOT_LIB}" -xpf "${archive}"
    fi
}

deploy_local_sysroot()
{
    local extracted="${WORKDIR}/host-bundle"
    local deployed_manifest="${WORKDIR}/sysroot-after.manifest"

    log "Creating a temporary local rollback snapshot."
    backup_local_sysroot

    rm -rf "${extracted}"
    mkdir -p "${extracted}"
    tar -C "${extracted}" -xpf "${BUNDLE}"
    cmp -s "${SOURCE_MANIFEST}" "${extracted}/source.manifest" ||
        die "Internal bundle manifest mismatch."
    generate_manifest "${extracted}/payload" "${extracted}/payload.manifest"
    cmp -s "${extracted}/source.manifest" "${extracted}/payload.manifest" ||
        die "Internal bundle payload verification failed."

    log "Cleaning old Lely libraries from local GDB sysroot."
    sudo mkdir -p "${SYSROOT_LIB}"
    HOST_CHANGED=1
    clean_local_sysroot

    log "Installing current stage libraries into local GDB sysroot."
    tar -C "${extracted}/payload" -cpf - . |
        sudo tar -C "${SYSROOT_LIB}" --no-same-owner -xpf -
    sudo find "${SYSROOT_LIB}" -mindepth 1 -maxdepth 1 \
        \( -type f -o -type l \) -name 'liblely-*.so*' \
        -exec chown -h root:root {} +

    generate_manifest "${SYSROOT_LIB}" "${deployed_manifest}"
    cmp -s "${SOURCE_MANIFEST}" "${deployed_manifest}" ||
        die "Local GDB sysroot verification failed."

    cp "${deployed_manifest}" "${LOCAL_BACKUP_DIR}/sysroot-after.manifest"
    log "Local GDB sysroot verification passed."
}

upload_bundle()
{
    local remote_bundle=$1

    log "Uploading deployment bundle to ${TARGET}:${remote_bundle}"
    remote_ssh "${TARGET}" "umask 077; cat > '${remote_bundle}'" <"${BUNDLE}"
}

deploy_target()
{
    local remote_bundle="/tmp/lely-libraries-${DEPLOY_ID}.tar"
    upload_bundle "${remote_bundle}"

    log "Creating a temporary target rollback snapshot, then cleaning and replacing Lely libraries."
    remote_ssh "${TARGET}" bash -s -- \
        "${remote_bundle}" \
        "${TARGET_LIB}" \
        "${REMOTE_BACKUP_DIR}" \
        "${TARGET_APP}" <<'REMOTE_DEPLOY'
set -Eeuo pipefail
IFS=$'\n\t'

bundle=$1
target_lib=$2
backup_dir=$3
target_app=$4

deploy_id=${backup_dir##*/}
work_dir="${target_lib}/.lely-deploy-${deploy_id}"
rollback_marker="/tmp/${deploy_id}.started"
changed=0

fail()
{
    printf '%s\n' "$*" >&2
    return 1
}

collect_names()
{
    find "$1" -mindepth 1 -maxdepth 1 \
        \( -type f -o -type l \) -name 'liblely-*.so*' -printf '%f\0' |
        LC_ALL=C sort -z
}

generate_manifest()
{
    local dir=$1
    local output=$2
    local name

    : >"${output}"
    while IFS= read -r -d '' name; do
        if [[ -L ${dir}/${name} ]]; then
            printf 'L\t%s\t%s\n' "${name}" "$(readlink "${dir}/${name}")" >>"${output}"
        else
            printf 'F\t%s\t%s\n' "${name}" \
                "$(sha256sum "${dir}/${name}" | awk '{print $1}')" >>"${output}"
        fi
    done < <(collect_names "${dir}")
}

clean_target()
{
    find "${target_lib}" -mindepth 1 -maxdepth 1 \
        \( -type f -o -type l \) -name 'liblely-*.so*' -delete
}

rollback()
{
    local rc=$?
    local restore_ok=1

    trap - ERR
    set +e
    if [[ -e ${rollback_marker} ]]; then
        exit "${rc}"
    fi
    : >"${rollback_marker}"

    if ((changed == 1)); then
        printf 'Target deployment failed; rolling back from %s\n' "${backup_dir}" >&2
        clean_target || restore_ok=0
        if [[ -d ${backup_dir}/files ]]; then
            tar -C "${backup_dir}/files" -cpf - . |
                tar -C "${target_lib}" -xpf - || restore_ok=0
        else
            restore_ok=0
        fi
        ldconfig || restore_ok=0
    fi
    if ((restore_ok == 1)); then
        rm -rf "${backup_dir}"
    else
        printf 'Target rollback was incomplete; temporary snapshot retained at %s\n' \
            "${backup_dir}" >&2
    fi
    rm -rf "${work_dir}" "${bundle}"
    exit "${rc}"
}

main_remote()
{
    local command_name resolved old_list

    for command_name in find sort tar sha256sum awk cmp readlink chown ldconfig ldd \
        mkdir rm cp; do
        if ! command -v "${command_name}" >/dev/null 2>&1; then
            fail "Required target command not found: ${command_name}"
        fi
    done

    if [[ -e ${backup_dir} ]]; then
        fail "Target temporary rollback directory already exists: ${backup_dir}"
    fi

    mkdir -p "${target_lib}" "${backup_dir}/files"
    rm -rf "${work_dir}"
    mkdir -p "${work_dir}"
    tar -C "${work_dir}" -xpf "${bundle}"

    generate_manifest "${work_dir}/payload" "${work_dir}/payload.manifest"
    if ! cmp -s "${work_dir}/source.manifest" "${work_dir}/payload.manifest"; then
        fail 'Uploaded bundle payload verification failed.'
    fi

    old_list="${work_dir}/target-before.list"
    collect_names "${target_lib}" >"${old_list}"
    generate_manifest "${target_lib}" "${backup_dir}/target-before.manifest"
    if [[ -s ${old_list} ]]; then
        tar -C "${target_lib}" --null -T "${old_list}" -cpf - |
            tar -C "${backup_dir}/files" -xpf -
    fi

    changed=1
    clean_target

    tar -C "${work_dir}/payload" -cpf - . |
        tar -C "${target_lib}" --no-same-owner -xpf -
    find "${target_lib}" -mindepth 1 -maxdepth 1 \
        \( -type f -o -type l \) -name 'liblely-*.so*' \
        -exec chown -h root:root {} +
    ldconfig

    generate_manifest "${target_lib}" "${work_dir}/target-after.manifest"
    if ! cmp -s "${work_dir}/source.manifest" "${work_dir}/target-after.manifest"; then
        fail 'Target manifest verification failed.'
    fi
    cp "${work_dir}/target-after.manifest" "${backup_dir}/target-after.manifest"

    if [[ -e ${target_app} ]]; then
        resolved=$(ldd "${target_app}" 2>/dev/null |
            awk '/liblely-io2\.so/{print $3; exit}')
        [[ -n ${resolved} ]] || fail "ldd did not resolve liblely-io2 for ${target_app}"
        resolved=$(readlink -f "${resolved}")
        case "${resolved}" in
            "${target_lib}"/*) ;;
            *)
                fail "Target executable resolves liblely-io2 outside ${target_lib}: ${resolved}"
                ;;
        esac
    fi

    changed=0
    rm -rf "${work_dir}" "${bundle}" "${rollback_marker}"
    printf 'Target deployment and verification passed.\n'
}

trap 'rm -f -- "${rollback_marker}"' EXIT
trap rollback ERR
main_remote
trap - ERR
trap - EXIT
rm -f -- "${rollback_marker}"
REMOTE_DEPLOY

    TARGET_CHANGED=1
}

restore_target()
{
    warn "Rolling back target libraries from the temporary snapshot."
    remote_ssh "${TARGET}" bash -s -- "${TARGET_LIB}" "${REMOTE_BACKUP_DIR}" <<'REMOTE_RESTORE'
set -Eeuo pipefail
IFS=$'\n\t'
target_lib=$1
backup_dir=$2

[[ -d ${backup_dir}/files ]] || {
    printf 'Target backup is missing: %s/files\n' "${backup_dir}" >&2
    exit 1
}

find "${target_lib}" -mindepth 1 -maxdepth 1 \
    \( -type f -o -type l \) -name 'liblely-*.so*' -delete

tar -C "${backup_dir}/files" -cpf - . |
    tar -C "${target_lib}" -xpf -
ldconfig
rm -rf "${backup_dir}"
REMOTE_RESTORE
}

discard_rollback_snapshots()
{
    log "Removing temporary rollback snapshots."
    if ! remote_ssh "${TARGET}" rm -rf -- "${REMOTE_BACKUP_DIR}"; then
        SNAPSHOT_CLEANUP_OK=0
        warn "Unable to remove target rollback snapshot: ${TARGET}:${REMOTE_BACKUP_DIR}"
    fi
    if ! rm -rf "${LOCAL_BACKUP_DIR}"; then
        SNAPSHOT_CLEANUP_OK=0
        warn "Unable to remove local rollback snapshot: ${LOCAL_BACKUP_DIR}"
    fi
}

verify_final_state()
{
    local local_manifest="${WORKDIR}/final-local.manifest"
    local target_manifest="${WORKDIR}/final-target.manifest"

    generate_manifest "${SYSROOT_LIB}" "${local_manifest}"
    cmp -s "${SOURCE_MANIFEST}" "${local_manifest}" ||
        die "Final local manifest differs from the stage manifest."

    remote_ssh "${TARGET}" bash -s -- "${TARGET_LIB}" >"${target_manifest}" <<'REMOTE_MANIFEST'
set -Eeuo pipefail
IFS=$'\n\t'
target_lib=$1
find "${target_lib}" -mindepth 1 -maxdepth 1 \
    \( -type f -o -type l \) -name 'liblely-*.so*' -printf '%f\0' |
    LC_ALL=C sort -z |
while IFS= read -r -d '' name; do
    if [[ -L ${target_lib}/${name} ]]; then
        printf 'L\t%s\t%s\n' "${name}" "$(readlink "${target_lib}/${name}")"
    else
        printf 'F\t%s\t%s\n' "${name}" \
            "$(sha256sum "${target_lib}/${name}" | awk '{print $1}')"
    fi
done
REMOTE_MANIFEST

    cmp -s "${SOURCE_MANIFEST}" "${target_manifest}" ||
        die "Final target manifest differs from the stage manifest."

    cp "${SOURCE_MANIFEST}" "${LOCAL_BACKUP_DIR}/source.manifest"
    cp "${target_manifest}" "${LOCAL_BACKUP_DIR}/target-after.manifest"
}

cleanup_workdir()
{
    if [[ -n ${WORKDIR} && -d ${WORKDIR} ]]; then
        if ((KEEP_WORKDIR == 1)); then
            warn "Keeping temporary work directory: ${WORKDIR}"
        else
            rm -rf "${WORKDIR}"
        fi
    fi
}

on_error()
{
    local rc=$?
    trap - ERR
    warn "Deployment failed with exit code ${rc}."

    if ((TARGET_CHANGED == 1)); then
        restore_target || warn "Target rollback failed; inspect ${REMOTE_BACKUP_DIR}."
    fi
    if ((HOST_CHANGED == 1)); then
        restore_local_sysroot || warn "Local sysroot rollback failed; inspect ${LOCAL_BACKUP_DIR}."
    fi

    cleanup_workdir
    exit "${rc}"
}

parse_arguments()
{
    while (($# > 0)); do
        case $1 in
            --apply)
                APPLY=1
                ;;
            --stage-lib)
                (($# >= 2)) || die "Missing value for --stage-lib"
                STAGE_LIB=$2
                shift
                ;;
            --sysroot-lib)
                (($# >= 2)) || die "Missing value for --sysroot-lib"
                SYSROOT_LIB=$2
                shift
                ;;
            --target)
                (($# >= 2)) || die "Missing value for --target"
                TARGET=$2
                shift
                ;;
            --target-port)
                (($# >= 2)) || die "Missing value for --target-port"
                TARGET_PORT=$2
                shift
                ;;
            --target-lib)
                (($# >= 2)) || die "Missing value for --target-lib"
                TARGET_LIB=$2
                shift
                ;;
            --target-app)
                (($# >= 2)) || die "Missing value for --target-app"
                TARGET_APP=$2
                shift
                ;;
            --keep-workdir)
                KEEP_WORKDIR=1
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                die "Unknown option: $1"
                ;;
        esac
        shift
    done
}

main()
{
    parse_arguments "$@"

    STAGE_LIB=$(normalize_dir "${STAGE_LIB}")
    SYSROOT_LIB=$(normalize_dir "${SYSROOT_LIB}")
    TARGET_LIB=$(normalize_dir "${TARGET_LIB}")
    [[ -n ${TARGET} ]] || die "SSH target must not be empty."
    [[ ${TARGET_PORT} =~ ^[0-9]+$ ]] || die "SSH port must be an integer: ${TARGET_PORT}"
    ((TARGET_PORT >= 1 && TARGET_PORT <= 65535)) ||
        die "SSH port must be in the range 1..65535: ${TARGET_PORT}"
    [[ ${TARGET_CONNECT_TIMEOUT} =~ ^[0-9]+$ ]] ||
        die "SSH connect timeout must be an integer: ${TARGET_CONNECT_TIMEOUT}"
    ((TARGET_CONNECT_TIMEOUT >= 1 && TARGET_CONNECT_TIMEOUT <= 300)) ||
        die "SSH connect timeout must be in the range 1..300: ${TARGET_CONNECT_TIMEOUT}"
    [[ ${STAGE_LIB} != "${SYSROOT_LIB}" ]] ||
        die "Stage and sysroot directories must be different."

    for command_name in find sort tar sha256sum readelf readlink awk ssh cmp grep sed date mktemp; do
        require_command "${command_name}"
    done
    if [[ -n ${TARGET_PASSWORD} ]]; then
        require_command sshpass
    fi

    DEPLOY_ID=$(date '+%Y%m%d-%H%M%S')-$$
    WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/lely-deploy.XXXXXXXX")
    trap cleanup_workdir EXIT

    validate_source_tree
    create_bundle

    log "Project directory: ${SCRIPT_DIR}"
    log "Workspace directory: ${WORKSPACE_DIR}"
    log "Source stage library directory: ${STAGE_LIB}"
    log "Source manifest (${SOURCE_MANIFEST}):"
    sed 's/^/  /' "${SOURCE_MANIFEST}"
    log "Source Build IDs:"
    print_build_ids "${STAGE_LIB}"

    scan_local_sysroot
    scan_target

    if ((APPLY == 0)); then
        log "Dry-run completed. No files were changed."
        log "Run '${SCRIPT_NAME} --apply' to deploy."
        return
    fi

    require_command sudo
    sudo -v

    LOCAL_BACKUP_DIR="${WORKDIR}/rollback-local"
    REMOTE_BACKUP_DIR="/tmp/lely-rollback-${DEPLOY_ID}"
    mkdir -p "${LOCAL_BACKUP_DIR}"
    cp "${SOURCE_MANIFEST}" "${LOCAL_BACKUP_DIR}/source.manifest"
    printf '%s\n' \
        "stage=${STAGE_LIB}" \
        "sysroot=${SYSROOT_LIB}" \
        "target=${TARGET}" \
        "target_port=${TARGET_PORT}" \
        "target_connect_timeout=${TARGET_CONNECT_TIMEOUT}" \
        "target_lib=${TARGET_LIB}" \
        "target_app=${TARGET_APP}" \
        "remote_rollback_snapshot=${REMOTE_BACKUP_DIR}" \
        >"${LOCAL_BACKUP_DIR}/deployment.env"

    trap on_error ERR
    deploy_local_sysroot
    deploy_target
    verify_final_state
    trap - ERR

    HOST_CHANGED=0
    TARGET_CHANGED=0
    discard_rollback_snapshots

    log "Deployment completed successfully."
    if ((SNAPSHOT_CLEANUP_OK == 1)); then
        log "No retained backup was created."
    else
        warn "Deployment succeeded, but a temporary rollback snapshot may remain."
    fi
    log "The stage, GDB sysroot, and target now contain an identical Lely library set."
    log "Restart canopen_master and gdbserver before debugging the new libraries."
}

main "$@"
