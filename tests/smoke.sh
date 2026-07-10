#!/bin/sh
set -eu

WRAP=${WRAP:-./microwrap}

case "$(uname -s)" in
Linux) ;;
*) echo "smoke tests require Linux" >&2; exit 77 ;;
esac

if [ ! -x "$WRAP" ]; then
    echo "missing executable: $WRAP" >&2
    exit 1
fi

tmp=$(mktemp -d)
outside_pid=
wrapper_pid=
cleanup()
{
    [ -z "$wrapper_pid" ] || kill "$wrapper_pid" 2>/dev/null || true
    [ -z "$outside_pid" ] || kill "$outside_pid" 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT INT HUP TERM
caller_uid=$(id -u)
caller_gid=$(id -g)

base_args='--ro-bind /bin /bin --ro-bind /usr /usr --ro-bind /lib /lib'
if [ -e /lib64 ]; then
    base_args="$base_args --ro-bind /lib64 /lib64"
fi

echo "default admin environment"
EXPECTED_UID=$caller_uid EXPECTED_GID=$caller_gid "$WRAP" -- /bin/sh -c '
    set -eu
    test "$(id -u)" = "$EXPECTED_UID"
    test "$(id -g)" = "$EXPECTED_GID"
    test "$(id -un)" = admin
    test "$(id -gn)" = admin
    test "$(getent passwd admin | cut -d: -f3)" = "$EXPECTED_UID"
    test "$(getent group admin | cut -d: -f3)" = "$EXPECTED_GID"
    test "$USER" = admin
    test "$LOGNAME" = admin
    test "$HOME" = /home/admin
    test "$PWD" = /home/admin
    test "$TMPDIR" = /tmp
    test "$XDG_RUNTIME_DIR" = "/run/user/$EXPECTED_UID"
    test "$(stat -c %a "$HOME")" = 700
    test "$(stat -c %a "$TMPDIR")" = 1777
    test "$(stat -c %a "$XDG_RUNTIME_DIR")" = 700
    test -d "$XDG_CONFIG_HOME"
    touch "$HOME/home-write" "$TMPDIR/tmp-write"
'
if [ -x /bin/bash ]; then
    EXPECTED_UID=$caller_uid EXPECTED_GID=$caller_gid "$WRAP" -- /bin/bash -c '
        test "$UID" = "$EXPECTED_UID"
        test "$EUID" = "$EXPECTED_UID"
        test "$(id -g)" = "$EXPECTED_GID"
        test "$(readlink /dev/fd)" = /proc/self/fd
        test -e /dev/stdin
        test -e /dev/stdout
        test -e /dev/stderr
        diff <(printf a) <(printf a)
    '
fi

echo "PID namespace and supervision"
sleep 30 &
outside_pid=$!
HOST_PID=$outside_pid "$WRAP" -- /bin/sh -c '
    set -eu
    test "$$" = 2
    test "$PPID" = 1
    test "$(cat /proc/1/comm)" = microwrap
    test ! -e "/proc/$HOST_PID/status"
    if kill -0 "$HOST_PID" 2>/dev/null; then
        exit 1
    fi
'
HOST_PID=$outside_pid "$WRAP" --share-pid -- /bin/sh -c '
    set -eu
    test -r "/proc/$HOST_PID/status"
    kill -0 "$HOST_PID"
'
kill "$outside_pid"
wait "$outside_pid" 2>/dev/null || true
outside_pid=

if "$WRAP" -- /bin/sh -c 'exit 42'; then
    echo "nonzero command unexpectedly succeeded" >&2
    exit 1
else
    test "$?" = 42
fi

"$WRAP" --bind "$tmp" /out -- /bin/sh -c '
    trap "echo TERM >/out/signal; exit 23" TERM
    echo ready >/out/ready
    while :; do sleep 1; done
' &
wrapper_pid=$!
i=0
while [ ! -f "$tmp/ready" ] && [ "$i" -lt 100 ]; do
    sleep 0.05
    i=$((i + 1))
done
test -f "$tmp/ready"
kill -TERM "$wrapper_pid"
if wait "$wrapper_pid"; then
    signal_status=0
else
    signal_status=$?
fi
wrapper_pid=
test "$signal_status" = 23
test "$(cat "$tmp/signal")" = TERM

"$WRAP" --bind /proc /hostproc --bind "$tmp" /out -- /bin/sh -c '
    set -eu
    sleep 30 & child=$!
    for status in /hostproc/[0-9]*/status; do
        nspid=$(grep "^NSpid:" "$status" || true)
        last=
        for value in $nspid; do last=$value; done
        if [ "$last" = "$child" ]; then
            host_pid=${status#/hostproc/}
            host_pid=${host_pid%/status}
            echo "$host_pid" >/out/descendant-pid
            break
        fi
    done
    test -s /out/descendant-pid
'
descendant_pid=$(cat "$tmp/descendant-pid")
if kill -0 "$descendant_pid" 2>/dev/null; then
    echo "descendant survived namespace init exit" >&2
    exit 1
fi

echo "proc, sys, and standard devices"
"$WRAP" -- /bin/sh -c '
    set -eu
    test -r /proc/self/status
    test -r /proc/meminfo
    test -r /proc/mounts
    test -r /sys/devices/system/cpu/possible
    df >/dev/null
    if command -v lscpu >/dev/null 2>&1; then
        lscpu >/dev/null
    fi
    test -c /dev/full
    if printf x > /dev/full 2>/tmp/full.err; then
        exit 1
    fi
    test -d /dev/pts
    test -c /dev/ptmx
    test -d /dev/shm
    touch /dev/shm/write-test
'
"$WRAP" -- /bin/sh -c '
    set -eu
    awk '\''$5 == "/proc/sys" && $6 ~ /^ro(,|$)/ { found = 1 }
          END { exit !found }'\'' /proc/self/mountinfo
    awk '\''$5 == "/proc/sysrq-trigger" && $6 ~ /^ro(,|$)/ { found = 1 }
          END { exit !found }'\'' /proc/self/mountinfo
    test -c /proc/kcore
    test "$(stat -c %t:%T /proc/kcore)" = 1:3
'

echo "standard runtime directories"
"$WRAP" -- /bin/sh -c '
    set -eu
    test -d /run
    test -d /var
    test -d /var/tmp
    test -d /mnt
    test "$(stat -c %a /var/tmp)" = 1777
    test "$(readlink /var/run)" = /run
    test "$(readlink /var/lock)" = /run/lock
'

"$WRAP" --dir /proc -- /bin/sh -c 'test ! -e /proc/self'
"$WRAP" --tmpfs /dev -- /bin/sh -c 'test ! -e /dev/null'

echo "default runtime configuration"
host_resolv=$(cksum < /etc/resolv.conf)
wrapped_resolv=$("$WRAP" -- /bin/sh -c 'cksum < /etc/resolv.conf')
test "$wrapped_resolv" = "$host_resolv"
"$WRAP" -- /bin/sh -c '
    test -s /etc/hosts
    getent hosts localhost >/dev/null
'
if [ -d /etc/ssl ]; then
    "$WRAP" -- /bin/sh -c 'test -d /etc/ssl'
fi
if [ -r /etc/ld.so.cache ]; then
    "$WRAP" -- /bin/sh -c 'test -r /etc/ld.so.cache'
fi
if [ -r /etc/ssl/certs/ca-certificates.crt ]; then
    "$WRAP" -- /bin/sh -c 'test -r /etc/ssl/certs/ca-certificates.crt'
fi
printf 'nameserver 192.0.2.1\n' > "$tmp/resolv.conf"
"$WRAP" --ro-bind "$tmp/resolv.conf" /etc/resolv.conf -- /bin/sh -c '
    test "$(cat /etc/resolv.conf)" = "nameserver 192.0.2.1"
'

echo "custom user and bound home"
mkdir -p "$tmp/joe-home"
MICROWRAP_LEAK=yes "$WRAP" \
    --user joe \
    --bind "$tmp/joe-home" /home/joe \
    --clearenv \
    --setenv PROJECT demo \
    --setenv EXPECTED_UID "$caller_uid" \
    --setenv EXPECTED_GID "$caller_gid" \
    -- /bin/sh -c '
        set -eu
        test "$(id -u)" = "$EXPECTED_UID"
        test "$(id -g)" = "$EXPECTED_GID"
        test "$(id -un)" = joe
        test "$(id -gn)" = joe
        test "$USER" = joe
        test "$HOME" = /home/joe
        test "$PWD" = /home/joe
        test "$PROJECT" = demo
        test -z "${MICROWRAP_LEAK+x}"
        touch from-wrapper
    '
test -f "$tmp/joe-home/from-wrapper"

echo "basic rootless filesystem"
# shellcheck disable=SC2086
"$WRAP" --no-defaults $base_args --tmpfs /tmp --chdir /tmp -- /bin/sh -c '
    test "$(pwd)" = /tmp
    touch ok
    test -f ok
    test ! -e /home/admin
    test ! -e /root
'

echo "read-only and writable bind mounts"
mkdir -p "$tmp/src" "$tmp/rw"
printf 'host\n' > "$tmp/src/file"
# shellcheck disable=SC2086
"$WRAP" $base_args \
    --ro-bind "$tmp/src" /ro \
    --bind "$tmp/rw" /rw \
    --tmpfs /tmp \
    -- /bin/sh -c '
        test "$(cat /ro/file)" = host
        echo wrapped > /rw/out
        if sh -c "echo nope > /ro/file" 2>/tmp/ro.err; then
            exit 1
        fi
    '
test "$(cat "$tmp/rw/out")" = wrapped
test "$(cat "$tmp/src/file")" = host

echo "absolute chdir validation"
if "$WRAP" --chdir relative -- /bin/true 2>"$tmp/chdir.err"; then
    echo "relative --chdir unexpectedly succeeded" >&2
    exit 1
fi
grep -q "path must be absolute" "$tmp/chdir.err"

echo "symlink target protection"
# shellcheck disable=SC2086
if "$WRAP" $base_args --symlink /tmp /link --tmpfs /link/escape -- /bin/true 2>"$tmp/symlink.err"; then
    echo "symlink mount target unexpectedly succeeded" >&2
    exit 1
fi
grep -q "refusing symlink" "$tmp/symlink.err"

echo "proc mount with --no-userns when privileged"
if "$WRAP" --no-userns $base_args --proc /proc -- /bin/sh -c 'test -d /proc/self' 2>"$tmp/proc.err"; then
    :
else
    echo "skipping privileged proc mount: $(cat "$tmp/proc.err")" >&2
fi

echo "ok"
