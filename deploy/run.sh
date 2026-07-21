#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd "$SCRIPT_DIR/.." && pwd)
# shellcheck source=deploy/ssh_common.sh
. "$SCRIPT_DIR/ssh_common.sh"

usage()
{
    cat <<'USAGE'
Usage:
  ./deploy/run.sh <target-name> <target-address> [run|gdb] [local-executable]

Environment variables:
  CANOPEN_TARGET_USER         SSH user; defaults to root
  CANOPEN_TARGET_PASSWORD     SSH password; SSH key authentication is used when empty
  CANOPEN_TARGET_SSH_PORT     SSH port; defaults to 22
  CANOPEN_SSH_CONNECT_TIMEOUT SSH connection timeout in seconds; defaults to 5
  CANOPEN_TARGET_PATH         Remote executable directory
  CANOPEN_TARGET_CONFIG_PATH  Remote runtime configuration directory
  CANOPEN_TESTER_CONFIG_PATH  Local tester.conf path
  CANOPEN_MASTER_DCF_PATH     Local master.dcf path
  CANOPEN_PROJECT_EDS_PATH    Local project.eds path
  CANOPEN_GDB_PORT            gdbserver port; defaults to 9091
USAGE
}

find_local_binary()
{
    target_name=$1
    for candidate in "./Output/$target_name" "./build/$target_name" "./$target_name"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

require_regular_file()
{
    description=$1
    file_path=$2

    if [ ! -f "$file_path" ]; then
        print_error "$description does not exist: $file_path"
        exit 4
    fi
    if [ ! -s "$file_path" ]; then
        print_error "$description is empty: $file_path"
        exit 4
    fi
}

normalize_remote_directory()
{
    directory_path=$1
    case "$directory_path" in
        /) printf '/\n' ;;
        */) printf '%s\n' "${directory_path%/}" ;;
        *) printf '%s\n' "$directory_path" ;;
    esac
}

cleanup_remote_staging()
{
    deploy_ssh "${DEPLOY_TARGET_USER}@${TARGET_IP}" sh -s -- \
        "$TARGET_PATH" "$TARGET_CONFIG_PATH" "$REMOTE_EXECUTABLE_UPLOAD" \
        "$REMOTE_TESTER_UPLOAD" "$REMOTE_MASTER_DCF_UPLOAD" "$REMOTE_PROJECT_EDS_UPLOAD" \
        >/dev/null 2>&1 <<'REMOTE_CLEANUP' || true
set -eu
target_path=$1
config_path=$2
remote_executable_upload=$3
remote_tester_upload=$4
remote_master_dcf_upload=$5
remote_project_eds_upload=$6

rm -f \
    "$target_path/$remote_executable_upload" \
    "$config_path/$remote_tester_upload" \
    "$config_path/$remote_master_dcf_upload" \
    "$config_path/$remote_project_eds_upload"
REMOTE_CLEANUP
}

cleanup_on_exit()
{
    command_status=$?
    trap - EXIT HUP INT TERM
    cleanup_remote_staging
    exit "$command_status"
}

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
    usage >&2
    exit 2
fi

TARGET_NAME=$1
TARGET_IP=$2
ACTION=${3:-run}
LOCAL_FILE_PATH=${4:-}
TARGET_PATH=$(normalize_remote_directory \
    "${CANOPEN_TARGET_PATH:-/opt/Ultra/Debug/canopen-slave-tester/bin}")
if [ -n "${CANOPEN_TARGET_CONFIG_PATH:-}" ]; then
    TARGET_CONFIG_PATH=$(normalize_remote_directory "$CANOPEN_TARGET_CONFIG_PATH")
else
    TARGET_CONFIG_PATH=$(normalize_remote_directory "$(dirname "$TARGET_PATH")/config")
fi
LOCAL_TESTER_CONFIG=${CANOPEN_TESTER_CONFIG_PATH:-$PROJECT_ROOT/config/tester.conf}
LOCAL_MASTER_DCF=${CANOPEN_MASTER_DCF_PATH:-$PROJECT_ROOT/config/generated/master.dcf}
LOCAL_PROJECT_EDS=${CANOPEN_PROJECT_EDS_PATH:-$PROJECT_ROOT/config/project.eds}
GDB_PORT=${CANOPEN_GDB_PORT:-9091}
DEPLOY_TOKEN=$$

log_info "Deployment started"

validate_positive_integer "CANOPEN_GDB_PORT" "$GDB_PORT" 1 65535

case "$TARGET_NAME" in
    ''|*[!A-Za-z0-9._-]*)
        print_error "The target name contains unsupported characters: $TARGET_NAME"
        exit 2
        ;;
esac
case "$ACTION" in
    run|gdb) ;;
    *)
        print_error "The action must be run or gdb: $ACTION"
        exit 2
        ;;
esac
case "$TARGET_PATH" in
    /*) ;;
    *)
        print_error "The remote executable directory must be an absolute path: $TARGET_PATH"
        exit 2
        ;;
esac
case "$TARGET_PATH" in
    *[!A-Za-z0-9._/-]*)
        print_error "The remote executable directory contains unsupported characters: $TARGET_PATH"
        exit 2
        ;;
esac
case "$TARGET_CONFIG_PATH" in
    /*) ;;
    *)
        print_error "The remote configuration directory must be an absolute path: $TARGET_CONFIG_PATH"
        exit 2
        ;;
esac
case "$TARGET_CONFIG_PATH" in
    *[!A-Za-z0-9._/-]*)
        print_error "The remote configuration directory contains unsupported characters: $TARGET_CONFIG_PATH"
        exit 2
        ;;
esac

if [ -z "$LOCAL_FILE_PATH" ]; then
    if ! LOCAL_FILE_PATH=$(find_local_binary "$TARGET_NAME"); then
        print_error "The build artifact was not found; pass the fourth argument or use ./Output/$TARGET_NAME or ./build/$TARGET_NAME"
        exit 4
    fi
fi

require_regular_file "The local executable" "$LOCAL_FILE_PATH"
require_regular_file "The local tester configuration" "$LOCAL_TESTER_CONFIG"
require_regular_file "The local master DCF" "$LOCAL_MASTER_DCF"
require_regular_file "The local project EDS" "$LOCAL_PROJECT_EDS"

validate_target_host "$TARGET_IP"
setup_ssh_connection "$TARGET_IP"

if [ "$ACTION" = "gdb" ]; then
    REMOTE_EXECUTABLE="${TARGET_NAME}.elf"
else
    REMOTE_EXECUTABLE=$TARGET_NAME
fi

REMOTE_EXECUTABLE_UPLOAD=".${REMOTE_EXECUTABLE}.upload.${DEPLOY_TOKEN}"
REMOTE_TESTER_UPLOAD=".tester.conf.upload.${DEPLOY_TOKEN}"
REMOTE_MASTER_DCF_UPLOAD=".master.dcf.upload.${DEPLOY_TOKEN}"
REMOTE_PROJECT_EDS_UPLOAD=".project.eds.upload.${DEPLOY_TOKEN}"

trap cleanup_on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

log_info "Target: ${DEPLOY_TARGET_USER}@${TARGET_IP}:${DEPLOY_TARGET_SSH_PORT}"
log_info "Remote executable directory: $TARGET_PATH"
log_info "Remote configuration directory: $TARGET_CONFIG_PATH"
log_info "Local executable: $LOCAL_FILE_PATH"
log_info "Local tester configuration: $LOCAL_TESTER_CONFIG"
log_info "Local master DCF: $LOCAL_MASTER_DCF"
log_info "Local project EDS: $LOCAL_PROJECT_EDS"
log_info "Action: $ACTION"
log_info "Preparing the target directories"

if run_logged deploy_ssh "${DEPLOY_TARGET_USER}@${TARGET_IP}" sh -s -- \
    "$TARGET_PATH" "$TARGET_CONFIG_PATH" <<'REMOTE_PREPARE'
set -eu
target_path=$1
config_path=$2

mkdir -p "$target_path/backup" "$config_path/backup"
REMOTE_PREPARE
then
    :
else
    command_status=$?
    print_error "Target preparation failed with exit code $command_status"
    exit "$command_status"
fi

upload_file()
{
    description=$1
    local_path=$2
    remote_path=$3

    log_info "Uploading $description"
    if run_logged deploy_scp "$local_path" "${DEPLOY_TARGET_USER}@${TARGET_IP}:$remote_path"; then
        return 0
    else
        command_status=$?
        print_error "$description upload failed with exit code $command_status"
        exit "$command_status"
    fi
}

upload_file "the executable" "$LOCAL_FILE_PATH" \
    "$TARGET_PATH/$REMOTE_EXECUTABLE_UPLOAD"
upload_file "tester.conf" "$LOCAL_TESTER_CONFIG" \
    "$TARGET_CONFIG_PATH/$REMOTE_TESTER_UPLOAD"
upload_file "master.dcf" "$LOCAL_MASTER_DCF" \
    "$TARGET_CONFIG_PATH/$REMOTE_MASTER_DCF_UPLOAD"
upload_file "project.eds" "$LOCAL_PROJECT_EDS" \
    "$TARGET_CONFIG_PATH/$REMOTE_PROJECT_EDS_UPLOAD"

log_info "Activating the uploaded executable and configuration files"
if run_logged deploy_ssh "${DEPLOY_TARGET_USER}@${TARGET_IP}" sh -s -- \
    "$TARGET_PATH" "$TARGET_CONFIG_PATH" "$TARGET_NAME" "$REMOTE_EXECUTABLE" \
    "$REMOTE_EXECUTABLE_UPLOAD" "$REMOTE_TESTER_UPLOAD" \
    "$REMOTE_MASTER_DCF_UPLOAD" "$REMOTE_PROJECT_EDS_UPLOAD" <<'REMOTE_ACTIVATE'
set -eu
target_path=$1
config_path=$2
target_name=$3
remote_executable=$4
remote_executable_upload=$5
remote_tester_upload=$6
remote_master_dcf_upload=$7
remote_project_eds_upload=$8

for staged_file in \
    "$target_path/$remote_executable_upload" \
    "$config_path/$remote_tester_upload" \
    "$config_path/$remote_master_dcf_upload" \
    "$config_path/$remote_project_eds_upload"
do
    if [ ! -s "$staged_file" ]; then
        printf 'Staged deployment file is missing or empty: %s\n' "$staged_file" >&2
        exit 4
    fi
done

timestamp=$(date +%Y%m%d_%H%M%S)
backup_file()
{
    source_path=$1
    backup_directory=$2

    if [ -f "$source_path" ]; then
        file_name=${source_path##*/}
        backup_path="$backup_directory/${file_name}.${timestamp}.bak"
        cp -p "$source_path" "$backup_path" 2>/dev/null || cp "$source_path" "$backup_path"
        printf 'Previous file backed up to: %s\n' "$backup_path"
    fi
}

backup_file "$target_path/$remote_executable" "$target_path/backup"
backup_file "$config_path/tester.conf" "$config_path/backup"
backup_file "$config_path/master.dcf" "$config_path/backup"
backup_file "$config_path/project.eds" "$config_path/backup"

chmod 0755 "$target_path/$remote_executable_upload"
chmod 0644 \
    "$config_path/$remote_tester_upload" \
    "$config_path/$remote_master_dcf_upload" \
    "$config_path/$remote_project_eds_upload"

killall "$target_name" 2>/dev/null || true
killall "${target_name}.elf" 2>/dev/null || true

mv -f "$config_path/$remote_tester_upload" "$config_path/tester.conf"
mv -f "$config_path/$remote_master_dcf_upload" "$config_path/master.dcf"
mv -f "$config_path/$remote_project_eds_upload" "$config_path/project.eds"
mv -f "$target_path/$remote_executable_upload" "$target_path/$remote_executable"
REMOTE_ACTIVATE
then
    :
else
    command_status=$?
    print_error "Deployment activation failed with exit code $command_status"
    exit "$command_status"
fi

trap - EXIT HUP INT TERM

if [ "$ACTION" = "gdb" ]; then
    log_info "Starting gdbserver on target port $GDB_PORT"
    log_info "Local debugger command: aarch64-poky-linux-gdb $LOCAL_FILE_PATH"
    log_info "GDB connection command: target remote ${TARGET_IP}:${GDB_PORT}"
    if run_logged deploy_ssh "${DEPLOY_TARGET_USER}@${TARGET_IP}" sh -s -- \
        "$TARGET_PATH" "$TARGET_CONFIG_PATH" "$REMOTE_EXECUTABLE" "$GDB_PORT" <<'REMOTE_GDB'
set -eu
target_path=$1
config_path=$2
remote_executable=$3
gdb_port=$4

cd "$target_path"
if command -v fuser >/dev/null 2>&1; then
    fuser -k "${gdb_port}/tcp" 2>/dev/null || true
fi
exec gdbserver ":$gdb_port" "./$remote_executable" \
    --config "$config_path/tester.conf"
REMOTE_GDB
    then
        :
    else
        command_status=$?
        print_error "gdbserver session failed with exit code $command_status"
        exit "$command_status"
    fi
else
    log_info "Starting the executable on the target board"
    if run_logged deploy_ssh "${DEPLOY_TARGET_USER}@${TARGET_IP}" sh -s -- \
        "$TARGET_PATH" "$TARGET_CONFIG_PATH" "$REMOTE_EXECUTABLE" <<'REMOTE_RUN'
set -eu
target_path=$1
config_path=$2
remote_executable=$3

cd "$target_path"
exec "./$remote_executable" --config "$config_path/tester.conf"
REMOTE_RUN
    then
        :
    else
        command_status=$?
        print_error "Remote execution failed with exit code $command_status"
        exit "$command_status"
    fi
fi

log_info "Deployment finished"
