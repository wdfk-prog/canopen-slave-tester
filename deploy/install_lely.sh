#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
DEPLOY_CONFIG_FILE=${CANOPEN_DEPLOY_CONFIG:-"$SCRIPT_DIR/local.conf"}
REMOTE_LIBRARY_DIR="/usr/local/lib"
REMOTE_LD_CONFIG_FILE="/etc/ld.so.conf.d/lely.conf"

usage()
{
    cat <<'USAGE'
Usage:
  ./deploy/install_lely.sh

Preparation:
  cp deploy/local.conf.example deploy/local.conf

  Then set the following values in deploy/local.conf:
    CANOPEN_TARGET_IP
    CANOPEN_LOCAL_LELY_ARCHIVE
    CANOPEN_TARGET_USER (optional, defaults to root)
    CANOPEN_TARGET_PASSWORD (optional; SSH key authentication is used when empty)
    CANOPEN_TARGET_SSH_PORT (optional, defaults to 22)

Description:
  This script accepts no positional arguments. It uploads a prebuilt Lely
  .tar.gz archive from the host, extracts it on the target board, installs the
  complete liblely-*.so* runtime set into /usr/local/lib, updates the dynamic
  linker configuration, and runs ldconfig.

Environment variables:
  CANOPEN_DEPLOY_CONFIG  Local configuration file; defaults to deploy/local.conf
USAGE
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
esac

if [ "$#" -ne 0 ]; then
    usage >&2
    exit 2
fi

if [ ! -r "$DEPLOY_CONFIG_FILE" ]; then
    printf 'ERROR: The local deployment configuration is not readable: %s\n' "$DEPLOY_CONFIG_FILE" >&2
    printf 'Copy deploy/local.conf.example to deploy/local.conf and update the local values.\n' >&2
    exit 2
fi

# shellcheck source=/dev/null
. "$DEPLOY_CONFIG_FILE"

# shellcheck source=deploy/ssh_common.sh
. "$SCRIPT_DIR/ssh_common.sh"

TARGET_IP=${CANOPEN_TARGET_IP:-}
LOCAL_ARCHIVE=${CANOPEN_LOCAL_LELY_ARCHIVE:-}
LEGACY_REMOTE_ARCHIVE=${CANOPEN_REMOTE_LELY_ARCHIVE:-}
REMOTE_UPLOAD_DIR=""
REMOTE_UPLOAD_ARCHIVE=""
REMOTE_UPLOAD_DIR_CREATED=false

cleanup_local()
{
    status=$?
    trap - 0 HUP INT TERM

    if [ "$REMOTE_UPLOAD_DIR_CREATED" = true ]; then
        log_info "Removing the temporary upload directory from the target board"
        set +e
        deploy_ssh "${DEPLOY_TARGET_USER}@${TARGET_IP}" rm -rf -- "$REMOTE_UPLOAD_DIR"
        set -e
    fi

    if [ "$status" -eq 0 ]; then
        log_info "Lely runtime library installation finished successfully"
    else
        log_error "Lely runtime library installation failed with exit code $status"
    fi
    exit "$status"
}
trap cleanup_local 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

log_info "Lely runtime library installation started"
log_info "Deployment configuration: $DEPLOY_CONFIG_FILE"

if [ -z "$TARGET_IP" ]; then
    print_error "CANOPEN_TARGET_IP is not set in the deployment configuration"
    exit 2
fi
if [ -z "$LOCAL_ARCHIVE" ]; then
    if [ -n "$LEGACY_REMOTE_ARCHIVE" ]; then
        print_error "CANOPEN_REMOTE_LELY_ARCHIVE is no longer supported"
        log_error "Rename it to CANOPEN_LOCAL_LELY_ARCHIVE and set it to the archive path on the host"
    else
        print_error "CANOPEN_LOCAL_LELY_ARCHIVE is not set in the deployment configuration"
    fi
    exit 2
fi
if [ ! -f "$LOCAL_ARCHIVE" ]; then
    print_error "The local Lely archive does not exist: $LOCAL_ARCHIVE"
    exit 4
fi
if [ ! -r "$LOCAL_ARCHIVE" ]; then
    print_error "The local Lely archive is not readable: $LOCAL_ARCHIVE"
    exit 4
fi

require_command tar
if ! tar -tzf "$LOCAL_ARCHIVE" >/dev/null 2>&1; then
    print_error "The local Lely archive is not a valid gzip-compressed tar archive: $LOCAL_ARCHIVE"
    exit 4
fi

validate_target_host "$TARGET_IP"
setup_ssh_connection "$TARGET_IP"

log_info "Target: ${DEPLOY_TARGET_USER}@${TARGET_IP}:${DEPLOY_TARGET_SSH_PORT}"
log_info "Local archive: $LOCAL_ARCHIVE"
log_info "Remote library directory: $REMOTE_LIBRARY_DIR"
log_info "Creating a secure temporary upload directory on the target board"

set +e
REMOTE_UPLOAD_DIR_OUTPUT=$(
    deploy_ssh "${DEPLOY_TARGET_USER}@${TARGET_IP}" sh -s <<'REMOTE_MKTEMP'
set -eu
umask 077
mktemp -d /tmp/canopen-lely-upload.XXXXXX
REMOTE_MKTEMP
)
command_status=$?
set -e
if [ "$command_status" -ne 0 ]; then
    print_error "Unable to create a secure temporary upload directory on the target board"
    exit "$command_status"
fi
REMOTE_UPLOAD_DIR=$REMOTE_UPLOAD_DIR_OUTPUT
case "$REMOTE_UPLOAD_DIR" in
    /tmp/canopen-lely-upload.[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]) ;;
    *)
        print_error "The target board returned an unexpected temporary directory: $REMOTE_UPLOAD_DIR"
        exit 4
        ;;
esac
REMOTE_UPLOAD_ARCHIVE="$REMOTE_UPLOAD_DIR/lely-runtime.tar.gz"
REMOTE_UPLOAD_DIR_CREATED=true
log_info "Temporary remote archive: $REMOTE_UPLOAD_ARCHIVE"
log_info "Uploading the Lely archive"

if run_logged deploy_scp "$LOCAL_ARCHIVE" \
    "${DEPLOY_TARGET_USER}@${TARGET_IP}:${REMOTE_UPLOAD_ARCHIVE}"; then
    :
else
    command_status=$?
    print_error "Archive upload failed with exit code $command_status"
    exit "$command_status"
fi

log_info "Starting the remote extraction and installation"
if run_logged deploy_ssh "${DEPLOY_TARGET_USER}@${TARGET_IP}" sh -s -- \
    "$REMOTE_UPLOAD_ARCHIVE" "$REMOTE_LIBRARY_DIR" "$REMOTE_LD_CONFIG_FILE" <<'REMOTE_INSTALL'
set -eu

archive=$1
library_dir=$2
ld_config_file=$3
staging_dir="/tmp/canopen-lely-install.$$"
extract_dir="$staging_dir/extracted"
directory_list="$staging_dir/directories.list"
backup_dir="/var/backups/canopen-lely/$(date +%Y%m%d_%H%M%S)_$$"
ld_config_backup="$staging_dir/lely.conf.backup"
ld_config_existed=false
install_started=false
install_complete=false
required_lely_libraries="liblely-coapp liblely-io2 liblely-ev liblely-co liblely-can liblely-util liblely-libc"

remote_log()
{
    level=$1
    shift
    printf '[%s] [REMOTE] [%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$level" "$*"
}

remote_error()
{
    remote_log ERROR "$@" >&2
}

has_lely_library()
{
    candidate_dir=$1
    library_name=$2

    for library in "$candidate_dir/$library_name.so"*; do
        if [ -f "$library" ] || [ -L "$library" ]; then
            return 0
        fi
    done
    return 1
}

has_required_lely_libraries()
{
    candidate_dir=$1

    for library_name in $required_lely_libraries; do
        if ! has_lely_library "$candidate_dir" "$library_name"; then
            return 1
        fi
    done
    return 0
}

find_lely_library_dir()
{
    for suffix in /stage/usr/lib /usr/lib /lib; do
        while IFS= read -r candidate_dir; do
            case "$candidate_dir" in
                *"$suffix")
                    if has_required_lely_libraries "$candidate_dir"; then
                        printf '%s\n' "$candidate_dir"
                        return 0
                    fi
                    ;;
            esac
        done < "$directory_list"
    done

    while IFS= read -r candidate_dir; do
        if has_required_lely_libraries "$candidate_dir"; then
            printf '%s\n' "$candidate_dir"
            return 0
        fi
    done < "$directory_list"

    return 1
}

restore_backup()
{
    rollback_status=0

    remote_log WARN "Restoring the previous Lely runtime libraries"
    if ! rm -f "$library_dir"/liblely-*.so*; then
        remote_error "Unable to remove the partially installed Lely runtime libraries"
        rollback_status=1
    fi

    for library in "$backup_dir"/liblely-*.so*; do
        if [ -L "$library" ]; then
            if ! cp -P "$library" "$library_dir/"; then
                remote_error "Unable to restore symbolic link: $library"
                rollback_status=1
            fi
        elif [ -f "$library" ]; then
            if ! cp -p "$library" "$library_dir/"; then
                remote_error "Unable to restore library: $library"
                rollback_status=1
            fi
        fi
    done

    if ! rm -f "$ld_config_file"; then
        remote_error "Unable to remove the updated dynamic linker configuration: $ld_config_file"
        rollback_status=1
    fi
    if [ "$ld_config_existed" = true ]; then
        if [ -L "$ld_config_backup" ]; then
            if ! cp -P "$ld_config_backup" "$ld_config_file"; then
                remote_error "Unable to restore the dynamic linker configuration symbolic link"
                rollback_status=1
            fi
        elif ! cp -p "$ld_config_backup" "$ld_config_file"; then
            remote_error "Unable to restore the dynamic linker configuration"
            rollback_status=1
        fi
    fi
    if ! ldconfig >/dev/null 2>&1; then
        remote_error "Unable to refresh the dynamic linker cache after rollback"
        rollback_status=1
    fi

    if [ "$rollback_status" -eq 0 ]; then
        remote_log WARN "The previous Lely runtime libraries were restored"
    else
        remote_error "Automatic rollback did not complete successfully"
    fi
    return "$rollback_status"
}

cleanup_remote()
{
    status=$?
    trap - 0 HUP INT TERM
    if [ "$install_started" = true ] && [ "$install_complete" != true ]; then
        remote_error "Installation failed; automatic rollback is starting"
        if restore_backup; then
            :
        else
            remote_error "Manual recovery is required; inspect the backup directory: $backup_dir"
        fi
    fi
    rm -rf "$staging_dir"
    rm -f "$archive"
    exit "$status"
}
trap cleanup_remote 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

remote_log INFO "Validating the uploaded archive and target environment"
if [ "$(id -u)" -ne 0 ]; then
    remote_error "Root privileges are required to install into $library_dir and run ldconfig"
    exit 3
fi
for command_name in tar find awk ldconfig grep; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        remote_error "Required command is not available on the target board: $command_name"
        exit 4
    fi
done
if [ ! -f "$archive" ]; then
    remote_error "The uploaded Lely archive does not exist: $archive"
    exit 4
fi
if [ ! -r "$archive" ]; then
    remote_error "The uploaded Lely archive is not readable: $archive"
    exit 4
fi
if ! tar -tzf "$archive" >/dev/null 2>&1; then
    remote_error "The uploaded Lely archive is not a valid gzip-compressed tar archive"
    exit 4
fi
if tar -tzf "$archive" | awk '
    /^\// || /(^|\/)\.\.(\/|$)/ { unsafe = 1; exit }
    END { exit unsafe ? 0 : 1 }
'; then
    remote_error "The uploaded Lely archive contains an unsafe path"
    exit 4
fi

remote_log INFO "Extracting the uploaded archive"
mkdir -p "$extract_dir" "$library_dir" "$backup_dir" "${ld_config_file%/*}"
tar -xzf "$archive" -C "$extract_dir"
find "$extract_dir" -type d -print > "$directory_list"

source_library_dir=$(find_lely_library_dir) || {
    remote_error "A directory containing the complete Lely runtime library set was not found in the archive"
    exit 5
}
remote_log INFO "Lely runtime library directory found: $source_library_dir"

if [ -L "$ld_config_file" ]; then
    cp -P "$ld_config_file" "$ld_config_backup"
    ld_config_existed=true
elif [ -f "$ld_config_file" ]; then
    cp -p "$ld_config_file" "$ld_config_backup"
    ld_config_existed=true
elif [ -e "$ld_config_file" ]; then
    remote_error "The dynamic linker configuration path is not a regular file or symbolic link: $ld_config_file"
    exit 4
fi

remote_log INFO "Backing up the currently installed Lely runtime libraries"
for library in "$library_dir"/liblely-*.so*; do
    if [ -e "$library" ] || [ -L "$library" ]; then
        if [ -L "$library" ]; then
            cp -P "$library" "$backup_dir/"
        else
            cp -p "$library" "$backup_dir/"
        fi
    fi
done

# Preserve unversioned linker names, SONAME links, and versioned shared objects.
(
    cd "$source_library_dir"
    set -- liblely-*.so*
    tar -cf "$staging_dir/lely-libraries.tar" "$@"
)

remote_log INFO "Installing the Lely runtime libraries into $library_dir"
install_started=true
rm -f "$library_dir"/liblely-*.so*
tar -xf "$staging_dir/lely-libraries.tar" -C "$library_dir"
chmod 755 "$library_dir"
for library in "$library_dir"/liblely-*.so*; do
    if [ -f "$library" ] && [ ! -L "$library" ]; then
        chmod 755 "$library"
    fi
done

if ! has_required_lely_libraries "$library_dir"; then
    remote_error "The installed Lely runtime library set is incomplete"
    exit 6
fi

remote_log INFO "Updating the dynamic linker configuration"
printf '%s\n' "$library_dir" > "$ld_config_file"
ldconfig

remote_log INFO "Verifying the dynamic linker cache"
for library_name in $required_lely_libraries; do
    if ! ldconfig -p | grep -F "$library_dir/$library_name.so" >/dev/null 2>&1; then
        remote_error "The dynamic linker cache does not contain $library_dir/$library_name.so*"
        exit 6
    fi
done

install_complete=true
remote_log INFO "Lely runtime libraries were installed successfully"
remote_log INFO "Library directory: $library_dir"
remote_log INFO "Dynamic linker configuration: $ld_config_file"
remote_log INFO "Previous-version backup: $backup_dir"
REMOTE_INSTALL
then
    :
else
    command_status=$?
    print_error "Remote Lely installation failed with exit code $command_status"
    exit "$command_status"
fi
