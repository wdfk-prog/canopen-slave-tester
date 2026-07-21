#!/usr/bin/env sh

# Shared SSH and console logging helpers for target-board deployment scripts.

DEPLOY_TARGET_USER=${CANOPEN_TARGET_USER:-root}
DEPLOY_TARGET_PASSWORD=${CANOPEN_TARGET_PASSWORD:-}
DEPLOY_TARGET_SSH_PORT=${CANOPEN_TARGET_SSH_PORT:-22}
DEPLOY_SSH_CONNECT_TIMEOUT=${CANOPEN_SSH_CONNECT_TIMEOUT:-5}

log_timestamp()
{
    date '+%Y-%m-%d %H:%M:%S'
}

log_message()
{
    log_level=$1
    shift
    log_line="[$(log_timestamp)] [$log_level] $*"

    case "$log_level" in
        ERROR|WARN)
            printf '%s\n' "$log_line" >&2
            ;;
        *)
            printf '%s\n' "$log_line"
            ;;
    esac
}

log_info()
{
    log_message INFO "$@"
}

log_warn()
{
    log_message WARN "$@"
}

log_error()
{
    log_message ERROR "$@"
}

print_error()
{
    log_error "$@"
}

require_command()
{
    if ! command -v "$1" >/dev/null 2>&1; then
        print_error "Required command is not available: $1"
        exit 2
    fi
}

validate_positive_integer()
{
    value_name=$1
    value=$2
    minimum=$3
    maximum=$4

    case "$value" in
        ''|*[!0-9]*)
            print_error "$value_name must be an integer in the range $minimum..$maximum: $value"
            exit 2
            ;;
    esac
    if [ "$value" -lt "$minimum" ] || [ "$value" -gt "$maximum" ]; then
        print_error "$value_name must be in the range $minimum..$maximum: $value"
        exit 2
    fi
}

validate_target_user()
{
    case "$DEPLOY_TARGET_USER" in
        ''|-*|*[!A-Za-z0-9._-]*)
            print_error "The SSH user contains unsupported characters: $DEPLOY_TARGET_USER"
            exit 2
            ;;
    esac
}

validate_target_host()
{
    target_host=$1
    validate_target_user
    validate_positive_integer "CANOPEN_TARGET_SSH_PORT" "$DEPLOY_TARGET_SSH_PORT" 1 65535
    validate_positive_integer "CANOPEN_SSH_CONNECT_TIMEOUT" "$DEPLOY_SSH_CONNECT_TIMEOUT" 1 300

    if [ -z "${HOME:-}" ]; then
        print_error "HOME is not set; the SSH known_hosts file cannot be maintained"
        exit 2
    fi
    if [ -z "$target_host" ] || [ "${target_host#-}" != "$target_host" ] \
        || ! printf '%s\n' "$target_host" | grep -Eq '^[A-Za-z0-9._-]+$'; then
        print_error "The target address must be an IPv4 address or hostname: $target_host"
        exit 2
    fi
}

run_logged()
{
    "$@"
}

setup_ssh_connection()
{
    target_host=$1
    validate_target_host "$target_host"

    require_command ssh
    require_command ssh-keygen
    require_command ssh-keyscan
    if [ -n "$DEPLOY_TARGET_PASSWORD" ]; then
        require_command sshpass
    fi

    mkdir -p "$HOME/.ssh"
    chmod 700 "$HOME/.ssh"
    touch "$HOME/.ssh/known_hosts"
    chmod 600 "$HOME/.ssh/known_hosts"

    require_command mktemp
    scan_directory=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/canopen-ssh-keyscan.XXXXXX") || {
        print_error "Unable to create a secure temporary directory for ssh-keyscan"
        exit 3
    }
    scan_output="$scan_directory/keys"
    scan_error="$scan_directory/errors"

    log_info "Checking the SSH service at ${target_host}:${DEPLOY_TARGET_SSH_PORT}"
    if ! ssh-keyscan -T "$DEPLOY_SSH_CONNECT_TIMEOUT" -p "$DEPLOY_TARGET_SSH_PORT" \
        -H "$target_host" > "$scan_output" 2> "$scan_error"; then
        if [ -s "$scan_error" ]; then
            while IFS= read -r error_line; do
                log_error "$error_line"
            done < "$scan_error"
        fi
        rm -rf "$scan_directory"
        print_error "The SSH service is not accepting connections at ${target_host}:${DEPLOY_TARGET_SSH_PORT}"
        log_error "Verify the target IPv4 address or hostname, Ethernet link, firewall, and the sshd or dropbear service"
        exit 3
    fi

    log_info "The SSH service is reachable"
    if [ "$DEPLOY_TARGET_SSH_PORT" -eq 22 ]; then
        known_host_name=$target_host
    else
        known_host_name="[$target_host]:$DEPLOY_TARGET_SSH_PORT"
    fi

    if ! ssh-keygen -F "$known_host_name" >/dev/null 2>&1; then
        log_info "Recording the SSH host key for $known_host_name"
        if ! cat "$scan_output" >> "$HOME/.ssh/known_hosts"; then
            rm -rf "$scan_directory"
            print_error "Unable to record the SSH host key in $HOME/.ssh/known_hosts"
            exit 3
        fi
    fi

    rm -rf "$scan_directory"
}

deploy_ssh()
{
    if [ -n "$DEPLOY_TARGET_PASSWORD" ]; then
        SSHPASS="$DEPLOY_TARGET_PASSWORD" sshpass -e ssh \
            -p "$DEPLOY_TARGET_SSH_PORT" \
            -o "ConnectTimeout=$DEPLOY_SSH_CONNECT_TIMEOUT" \
            -o ConnectionAttempts=1 "$@"
    else
        ssh -p "$DEPLOY_TARGET_SSH_PORT" \
            -o "ConnectTimeout=$DEPLOY_SSH_CONNECT_TIMEOUT" \
            -o ConnectionAttempts=1 "$@"
    fi
}

deploy_ssh_tty()
{
    if [ -n "$DEPLOY_TARGET_PASSWORD" ]; then
        SSHPASS="$DEPLOY_TARGET_PASSWORD" sshpass -e ssh -t \
            -p "$DEPLOY_TARGET_SSH_PORT" \
            -o "ConnectTimeout=$DEPLOY_SSH_CONNECT_TIMEOUT" \
            -o ConnectionAttempts=1 "$@"
    else
        ssh -t -p "$DEPLOY_TARGET_SSH_PORT" \
            -o "ConnectTimeout=$DEPLOY_SSH_CONNECT_TIMEOUT" \
            -o ConnectionAttempts=1 "$@"
    fi
}

deploy_scp()
{
    require_command scp
    if [ -n "$DEPLOY_TARGET_PASSWORD" ]; then
        SSHPASS="$DEPLOY_TARGET_PASSWORD" sshpass -e scp \
            -P "$DEPLOY_TARGET_SSH_PORT" \
            -o "ConnectTimeout=$DEPLOY_SSH_CONNECT_TIMEOUT" \
            -o ConnectionAttempts=1 "$@"
    else
        scp -P "$DEPLOY_TARGET_SSH_PORT" \
            -o "ConnectTimeout=$DEPLOY_SSH_CONNECT_TIMEOUT" \
            -o ConnectionAttempts=1 "$@"
    fi
}
