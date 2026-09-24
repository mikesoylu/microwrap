# microwrap

`microwrap` is a tiny (~25 KiB gzipped) `bubblewrap` alternative. It implements the
essential parts of `bubblewrap`: create private namespaces, build a minimal
filesystem, map folders, then run a command.

It is intentionally not a full sandbox. There is no general seccomp policy,
cgroups, device policy, or desktop/session integration. The only seccomp filter
blocks terminal input injection. Optional network modes provide clean
per-wrapper port and interface namespaces rather than a security boundary.

## Install

Install the latest tested release to `/usr/local/bin` as root:

```sh
curl -fsSL https://raw.githubusercontent.com/mikesoylu/microwrap/main/setup.sh | sh
```

For non-root user-local installation and release selection see [`setup.sh`](setup.sh).

## Usage

```text
microwrap [options] -- command [args...]

Filesystem options:
  --bind SRC DST       Bind-mount SRC read-write at absolute path DST.
  --ro-bind SRC DST    Bind-mount SRC read-only at absolute path DST.
  --tmpfs DST          Mount an empty tmpfs at absolute path DST.
  --tmpfs-size SIZE    Size limit for the root and each --tmpfs, such as 512m,
                       2g, or 25%. Defaults to the kernel's 50% of RAM.
  --proc DST           Mount procfs at absolute path DST.
  --dir DST            Create a directory at absolute path DST.
  --symlink TARGET DST Create symlink DST -> TARGET inside the wrapper.

Runtime options:
  --user NAME          Cosmetic account name. Defaults to admin.
  --home DIR           Home path. Defaults to /home/NAME.
  --chdir DIR          chdir before exec. Defaults to the home path.
  --setenv NAME VALUE  Set an environment variable before exec.
  --unsetenv NAME      Remove an environment variable before exec.
  --clearenv           Clear inherited variables before adding defaults.
  --no-defaults        Disable automatic mounts, account files, and env.
  --no-userns          Do not create a user namespace. Requires CAP_SYS_ADMIN.
  --share-pid          Share the caller's PID namespace and procfs.
  --share-ipc          Share the caller's SysV IPC and POSIX message queues.
  --network MODE       Network mode: host, none, or internet. Defaults to host.
  --help               Print the online usage URL.
```

By default, `microwrap` creates a user namespace and maps the caller's numeric
UID and primary GID to the same IDs inside it. It then creates private mount,
IPC, and PID namespaces and mounts a fresh tmpfs as the new root. A small
microwrap process runs as PID 1, forwards signals, reaps orphaned children, and
supervises the requested command as PID 2. Microwrap maps the host's `/usr` and
loader paths read-only, mounts a private procfs, binds selected sysfs views, and
provides standard devices, fd links, a private devpts instance, and shared
memory. `/tmp`, `/run`, `/var/tmp`, `/dev/shm`, and `/home/admin` are fresh
directories on the root tmpfs, so they share one size limit. It writes minimal
passwd/group data, maps common DNS, certificate, loader, and timezone
configuration from `/etc`, and sets a login-like environment. An explicit
operation at one of those destinations replaces that default. After setup it
chroots, joins a new session keyring, drops capabilities, sets `no_new_privs`,
installs a seccomp filter that blocks terminal input injection, then forks and
execs the command. If microwrap itself is killed, the kernel kills the wrapper
with it.

## Build

```sh
make
```

For a size-optimized, stripped binary:

```sh
make release
```

The terminal ioctl filter must list every syscall ABI of the target CPU family,
so microwrap builds for x86 (x86_64, i386, x32), ARM (aarch64, arm),
RISC-V, PowerPC, s390, and LoongArch. Other architectures fail to compile until
their ABIs are added to `install_tty_filter` in `microwrap.c`.

## Test

The tests need a Linux kernel with namespace and mount support. From a
non-Linux development machine, Docker works:

```sh
docker run --rm --privileged -v "$PWD:/work" -w /work debian:trixie-slim \
  sh -lc 'apt-get update &&
          apt-get install -y --no-install-recommends build-essential slirp4netns &&
          make clean &&
          make CFLAGS="-O2 -Wall -Wextra -Werror -std=c11" &&
          ./test.sh'
```

## Examples

These examples assume `microwrap` is installed on `PATH`.

Open a shell as the default cosmetic `admin` user with a temporary home:

```sh
microwrap -- /bin/sh
```

Use a different account name and replace its temporary home with a writable
host directory:

```sh
microwrap \
  --user joe \
  --bind /mnt/joeshome /home/joe \
  -- /bin/sh
```

Use the original empty-root behavior and specify every mapping yourself:

```sh
microwrap \
  --no-defaults \
  --ro-bind /usr /usr \
  --ro-bind /bin /bin \
  --ro-bind /lib /lib \
  --bind /dev/null /dev/null \
  --tmpfs /tmp \
  --bind "$PWD" /work \
  --chdir /work \
  -- /bin/sh
```

If you are running with real `CAP_SYS_ADMIN`, you can skip the user namespace
while retaining the default mount and PID isolation:

```sh
microwrap \
  --no-userns \
  -- /bin/sh -c 'ls /proc/self'
```

Give a process its own interfaces and port space with no external network:

```sh
microwrap --network none -- /bin/sh
```

Give it an isolated port space with outbound IPv4 Internet access. This mode
requires `slirp4netns` 0.4.0 or later, built with seccomp support, on the host:

```sh
microwrap --network internet -- curl https://example.com
```

## Notes

- `--bind` and `--ro-bind` are non-recursive. Submounts under `SRC` are not
  copied into the wrapper.
- `--ro-bind` remounts the bind target read-only. It is intentionally simple
  and does not use the newer recursive mount-attribute API.
- Rootless mode requires unprivileged user namespaces to be enabled by the
  kernel and distribution policy.
- `--network host` preserves the caller's network namespace and is the default.
  `--network none` creates a private namespace with no externally connected
  interface and brings up its private loopback device.
  `--network internet` adds a private TAP interface and outbound IPv4 access
  through `slirp4netns`; it fails with a diagnostic when the helper is absent.
- Private, link-local, shared, documentation, benchmarking, multicast, and
  reserved IPv4 destinations are unreachable in `internet` mode. Sandbox-local
  loopback remains usable. The policy is intended for predictable environments,
  not as a hardened firewall against a hostile process.
- `slirp4netns` runs outside the wrapper with its own seccomp filter, unless
  seccomp filters are unavailable. For root callers it also runs in its own
  mount sandbox with capabilities dropped, as long as `/etc/resolv.conf`
  resolves inside `/etc` or `/run`.
- Listening sockets in `none` and `internet` modes do not occupy or publish host
  ports. Internet connections necessarily use transient host-side client sockets
  in `slirp4netns`. There is no automatic inbound port forwarding.
- `--user` is cosmetic. The process keeps the caller's numeric UID and primary
  GID; the name, home, account files, working directory, and environment change.
- Supplementary host groups are not mapped into the user namespace.
- The default `/proc` belongs to the new PID namespace. Sensitive kernel paths
  are masked and `/proc/sys`, `/proc/sysrq-trigger`, `/proc/irq`, `/proc/bus`,
  and `/proc/fs` are read-only. Use `--dir /proc` to leave procfs empty.
- `--share-pid` disables PID isolation and recursively binds the caller's
  procfs, including its existing masked and read-only submounts. This exposes
  host PIDs and restores the pre-PID-namespace behavior.
- The requested command runs as PID 2 under microwrap's minimal PID 1. When the
  command exits, remaining processes in the PID namespace are terminated. The
  same happens if microwrap is killed, even with `SIGKILL`; with `--share-pid`
  only the command itself is killed. The command cannot ptrace or inspect its
  PID 1.
- Selected `/sys` subtrees are bound for CPU, device, and cgroup introspection.
  Use `--tmpfs /sys` or `--no-defaults` to hide them.
- `/dev/pts` is a private devpts instance and `/dev/ptmx` links to it, so the
  wrapper can allocate its own terminals but cannot open the caller's other
  terminals. A terminal inherited on stdio still works, but its name cannot be
  resolved inside the wrapper, so `tty` reports `not a tty`. Use
  `--bind /dev/pts /dev/pts` to restore the host devpts.
- `/dev/tty` is available, but opening it still requires the caller to have
  supplied a controlling terminal, such as Docker's `-it` mode.
- A seccomp filter rejects the `TIOCSTI` and `TIOCLINUX` ioctls with `EPERM` for
  every syscall ABI the CPU supports, so a wrapped process cannot type commands
  into the caller's terminal. Where seccomp filters are unavailable, for example
  under qemu-user or Rosetta emulation, the command runs in a new session
  instead. The kernel then rejects `TIOCSTI` too, but interactive shells lose
  job control.
- Common runtime configuration is mapped read-only from `/etc`, including DNS,
  hosts, NSS network databases, certificates, loader configuration, and
  timezone data. Unrelated files such as host keys and credentials remain
  hidden unless explicitly mapped.
- The writable root and generated `/etc` are private tmpfs state and disappear
  when the wrapped command exits; `/usr` and mapped runtime configuration remain
  read-only. Setuid execution cannot elevate privileges because `no_new_privs`
  is set before exec.
- The root tmpfs, which holds `/tmp`, `/var/tmp`, `/dev/shm`, `/run`, and home,
  is limited to the kernel default of 50% of RAM, or `--tmpfs-size`. Each
  `--tmpfs` is a separate mount with its own limit.
- The wrapper gets a new, empty session keyring, so keys in the caller's session
  keyring are not reachable. With a user namespace on Linux 5.3 or later, the
  user keyring is separate too; with `--no-userns` the caller's user keyring
  remains reachable.
- SysV IPC and POSIX message queues are private unless `--share-ipc` is given.
  X11 clients that rely on MIT-SHM may need `--share-ipc`.
- The process still shares the host UTS namespace. It shares the host network
  namespace only in the default `host` network mode.
