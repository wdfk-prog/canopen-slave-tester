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
  CANOPEN_TARGET_USER          SSH user; defaults to root
  CANOPEN_TARGET_PASSWORD      SSH password; key authentication is used when empty
  CANOPEN_TARGET_SSH_PORT      SSH port; defaults to 22
  CANOPEN_SSH_CONNECT_TIMEOUT  SSH connection timeout in seconds; defaults to 5
  CANOPEN_TARGET_PATH          Remote executable directory
  CANOPEN_TARGET_CONFIG_PATH   Remote CANopen configuration directory
  CANOPEN_MASTER_DCF_PATH      Local master.dcf path
  CANOPEN_PROJECT_EDS_PATH     Local project.eds path
  CANOPEN_MCU_NODE_DCF_PATH    Local mcu_node_1.bin path
  CANOPEN_GDB_PORT             gdbserver port; defaults to 9091
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
        "$TARGET_PATH" "$TARGET_CONFIG_PATH" \
        "$REMOTE_EXECUTABLE_UPLOAD" "$REMOTE_MASTER_DCF_UPLOAD" \
        "$REMOTE_PROJECT_EDS_UPLOAD" "$REMOTE_MCU_NODE_DCF_UPLOAD" \
        >/dev/null 2>&1 <<'REMOTE_CLEANUP' || true
set -eu
target_path=$1
config_path=$2
remote_executable_upload=$3
remote_master_dcf_upload=$4
remote_project_eds_upload=$5
remote_mcu_node_dcf_upload=$6

rm -f \
    "$target_path/$remote_executable_upload" \
    "$config_path/$remote_master_dcf_upload" \
    "$config_path/$remote_project_eds_upload" \
    "$config_path/$remote_mcu_node_dcf_upload"
REMOTE_CLEANUP
}

cleanup_on_exit()
{
    command_status=$?
    trap - 0 HUP INT TERM
    cleanup_remote_staging
    exit "$command_status"
}

upload_file()
{
    description=$1
    local_path=$2
    remote_path=$3

    log_info "Uploading $description"
    if run_logged deploy_scp "$local_path" \
        "${DEPLOY_TARGET_USER}@${TARGET_IP}:$remote_path"; then
        return 0
    else
        command_status=$?
        print_error "$description upload failed with exit code $command_status"
        exit "$command_status"
    fi
}

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
    usage >&2
    exit 2
fi

TARGET_NAME=$1
TARGET_IP=$2
ACTION=${3:-run}
LOCAL_FILE_PATH=${4:-}

case "$TARGET_NAME" in
    ''|.|..|*[!A-Za-z0-9._-]*)
        print_error "The target name contains unsupported characters: $TARGET_NAME"
        exit 2
        ;;
esac

case "$ACTION" in
    run|gdb) ;;
    *)
        print_error "Unsupported action: $ACTION"
        usage >&2
        exit 2
        ;;
esac

TARGET_PATH=$(normalize_remote_directory \
    "${CANOPEN_TARGET_PATH:-/opt/Ultra/Debug/canopen-master/bin}")
if [ -n "${CANOPEN_TARGET_CONFIG_PATH:-}" ]; then
    TARGET_CONFIG_PATH=$(normalize_remote_directory \
        "$CANOPEN_TARGET_CONFIG_PATH")
else
    TARGET_CONFIG_PATH=$(normalize_remote_directory \
        "$(dirname "$TARGET_PATH")/config")
fi

LOCAL_MASTER_DCF=${CANOPEN_MASTER_DCF_PATH:-$PROJECT_ROOT/config/generated/master.dcf}
LOCAL_PROJECT_EDS=${CANOPEN_PROJECT_EDS_PATH:-$PROJECT_ROOT/config/project.eds}
LOCAL_MCU_NODE_DCF=${CANOPEN_MCU_NODE_DCF_PATH:-$PROJECT_ROOT/config/generated/mcu_node_1.bin}
GDB_PORT=${CANOPEN_GDB_PORT:-9091}

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
case "$GDB_PORT" in
    ''|*[!0-9]*)
        print_error "The gdbserver port must be a decimal number: $GDB_PORT"
        exit 2
        ;;
esac

if [ -z "$LOCAL_FILE_PATH" ]; then
    if ! LOCAL_FILE_PATH=$(find_local_binary "$TARGET_NAME"); then
        print_error "The build artifact was not found; pass the fourth argument"
        exit 4
    fi
fi

require_regular_file "The local executable" "$LOCAL_FILE_PATH"
require_regular_file "The local master DCF" "$LOCAL_MASTER_DCF"
require_regular_file "The local project EDS" "$LOCAL_PROJECT_EDS"
require_regular_file "The local MCU concise DCF" "$LOCAL_MCU_NODE_DCF"

validate_target_host "$TARGET_IP"
setup_ssh_connection "$TARGET_IP"

# Generate a unique suffix for remote staging files.
DEPLOY_TOKEN="$(date +%Y%m%d%H%M%S).$$"

if [ "$ACTION" = "gdb" ]; then
    REMOTE_EXECUTABLE="${TARGET_NAME}.elf"
else
    REMOTE_EXECUTABLE=$TARGET_NAME
fi
REMOTE_EXECUTABLE_UPLOAD=".${REMOTE_EXECUTABLE}.upload.${DEPLOY_TOKEN}"
REMOTE_MASTER_DCF_UPLOAD=".master.dcf.upload.${DEPLOY_TOKEN}"
REMOTE_PROJECT_EDS_UPLOAD=".project.eds.upload.${DEPLOY_TOKEN}"
REMOTE_MCU_NODE_DCF_UPLOAD=".mcu_node_1.bin.upload.${DEPLOY_TOKEN}"

trap cleanup_on_exit 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

log_info "Target: ${DEPLOY_TARGET_USER}@${TARGET_IP}:${DEPLOY_TARGET_SSH_PORT}"
log_info "Remote executable directory: $TARGET_PATH"
log_info "Remote configuration directory: $TARGET_CONFIG_PATH"
log_info "Local executable: $LOCAL_FILE_PATH"
log_info "Local master DCF: $LOCAL_MASTER_DCF"
log_info "Local project EDS: $LOCAL_PROJECT_EDS"
log_info "Local MCU concise DCF: $LOCAL_MCU_NODE_DCF"
log_info "Action: $ACTION"

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

upload_file "executable" "$LOCAL_FILE_PATH" \
    "$TARGET_PATH/$REMOTE_EXECUTABLE_UPLOAD"
upload_file "master.dcf" "$LOCAL_MASTER_DCF" \
    "$TARGET_CONFIG_PATH/$REMOTE_MASTER_DCF_UPLOAD"
upload_file "project.eds" "$LOCAL_PROJECT_EDS" \
    "$TARGET_CONFIG_PATH/$REMOTE_PROJECT_EDS_UPLOAD"
upload_file "mcu_node_1.bin" "$LOCAL_MCU_NODE_DCF" \
    "$TARGET_CONFIG_PATH/$REMOTE_MCU_NODE_DCF_UPLOAD"

log_info "Backing up and replacing the remote runtime set"
if run_logged deploy_ssh "${DEPLOY_TARGET_USER}@${TARGET_IP}" sh -s -- \
    "$TARGET_PATH" "$TARGET_CONFIG_PATH" "$REMOTE_EXECUTABLE" \
    "$REMOTE_EXECUTABLE_UPLOAD" "$REMOTE_MASTER_DCF_UPLOAD" \
    "$REMOTE_PROJECT_EDS_UPLOAD" "$REMOTE_MCU_NODE_DCF_UPLOAD" \
    <<'REMOTE_INSTALL'
set -eu
target_path=$1
config_path=$2
remote_executable=$3
remote_executable_upload=$4
remote_master_dcf_upload=$5
remote_project_eds_upload=$6
remote_mcu_node_dcf_upload=$7
backup_stamp=$(date +%Y%m%d-%H%M%S)
rollback_token="rollback.$$"
install_complete=false

executable_path="$target_path/$remote_executable"
master_dcf_path="$config_path/master.dcf"
project_eds_path="$config_path/project.eds"
mcu_node_dcf_path="$config_path/mcu_node_1.bin"

executable_rollback="$target_path/.${remote_executable}.${rollback_token}"
master_dcf_rollback="$config_path/.master.dcf.${rollback_token}"
project_eds_rollback="$config_path/.project.eds.${rollback_token}"
mcu_node_dcf_rollback="$config_path/.mcu_node_1.bin.${rollback_token}"

executable_existed=false
master_dcf_existed=false
project_eds_existed=false
mcu_node_dcf_existed=false

rollback_install()
{
    if [ "$install_complete" = true ]; then
        return
    fi

    if [ "$executable_existed" = true ]; then
        mv -f "$executable_rollback" "$executable_path" || true
    else
        rm -f "$executable_path"
    fi
    if [ "$master_dcf_existed" = true ]; then
        mv -f "$master_dcf_rollback" "$master_dcf_path" || true
    else
        rm -f "$master_dcf_path"
    fi
    if [ "$project_eds_existed" = true ]; then
        mv -f "$project_eds_rollback" "$project_eds_path" || true
    else
        rm -f "$project_eds_path"
    fi
    if [ "$mcu_node_dcf_existed" = true ]; then
        mv -f "$mcu_node_dcf_rollback" "$mcu_node_dcf_path" || true
    else
        rm -f "$mcu_node_dcf_path"
    fi
}

rollback_and_exit()
{
    signal_status=$1
    trap - 0 HUP INT TERM
    rollback_install
    exit "$signal_status"
}

trap rollback_install 0
trap 'rollback_and_exit 129' HUP
trap 'rollback_and_exit 130' INT
trap 'rollback_and_exit 143' TERM

if [ -e "$executable_path" ]; then
    executable_existed=true
    cp -p "$executable_path" \
        "$target_path/backup/$(basename "$executable_path").$backup_stamp"
    cp -p "$executable_path" "$executable_rollback"
fi
if [ -e "$master_dcf_path" ]; then
    master_dcf_existed=true
    cp -p "$master_dcf_path" \
        "$config_path/backup/$(basename "$master_dcf_path").$backup_stamp"
    cp -p "$master_dcf_path" "$master_dcf_rollback"
fi
if [ -e "$project_eds_path" ]; then
    project_eds_existed=true
    cp -p "$project_eds_path" \
        "$config_path/backup/$(basename "$project_eds_path").$backup_stamp"
    cp -p "$project_eds_path" "$project_eds_rollback"
fi
if [ -e "$mcu_node_dcf_path" ]; then
    mcu_node_dcf_existed=true
    cp -p "$mcu_node_dcf_path" \
        "$config_path/backup/$(basename "$mcu_node_dcf_path").$backup_stamp"
    cp -p "$mcu_node_dcf_path" "$mcu_node_dcf_rollback"
fi

chmod 755 "$target_path/$remote_executable_upload"
chmod 644 \
    "$config_path/$remote_master_dcf_upload" \
    "$config_path/$remote_project_eds_upload" \
    "$config_path/$remote_mcu_node_dcf_upload"

# Each rename is atomic because staging and destination files are on the same
# target filesystem. The rollback copies restore the previous complete set if
# any replacement in this remote transaction fails.
mv -f "$target_path/$remote_executable_upload" "$executable_path"
mv -f "$config_path/$remote_master_dcf_upload" "$master_dcf_path"
mv -f "$config_path/$remote_project_eds_upload" "$project_eds_path"
mv -f "$config_path/$remote_mcu_node_dcf_upload" "$mcu_node_dcf_path"

install_complete=true
trap - 0 HUP INT TERM
rm -f \
    "$executable_rollback" \
    "$master_dcf_rollback" \
    "$project_eds_rollback" \
    "$mcu_node_dcf_rollback"
REMOTE_INSTALL
then
    :
else
    command_status=$?
    print_error "Remote installation failed with exit code $command_status"
    exit "$command_status"
fi

trap - 0 HUP INT TERM

if [ "$ACTION" = "gdb" ]; then
    log_info "Starting gdbserver on port $GDB_PORT"
    deploy_ssh_tty "${DEPLOY_TARGET_USER}@${TARGET_IP}" \
        "cd '$TARGET_PATH' && exec gdbserver ':$GDB_PORT' './$REMOTE_EXECUTABLE'"
else
    log_info "Starting $REMOTE_EXECUTABLE"
    deploy_ssh_tty "${DEPLOY_TARGET_USER}@${TARGET_IP}" \
        "cd '$TARGET_PATH' && exec './$REMOTE_EXECUTABLE'"
fi
