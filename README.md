# microwrap

`microwrap` is a tiny Linux-only filesystem wrapper inspired by the small,
useful part of `bubblewrap`: create a private mount namespace, build a minimal
root filesystem, map folders, then run a command. It provides practical shell
defaults while keeping every filesystem mapping explicit or replaceable.

It is intentionally not a full sandbox. There is no seccomp, cgroups, device
policy, network setup, PID namespace handling, or desktop/session integration.

## Build

```sh
make
```

For a size-optimized, stripped binary:

```sh
make release
```

Release builds use 4 KiB ELF page alignment to avoid substantial padding on
ARM64. For a Linux target with 64 KiB pages, build with:

```sh
make release RELEASE_MAX_PAGE_SIZE=65536
```

The only build requirement is a Linux C toolchain and libc headers.

## Usage

```text
microwrap [options] -- command [args...]

Filesystem options:
  --bind SRC DST       Bind-mount SRC read-write at absolute path DST.
  --ro-bind SRC DST    Bind-mount SRC read-only at absolute path DST.
  --tmpfs DST          Mount an empty tmpfs at absolute path DST.
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
  --help               Show usage.
```

By default, `microwrap` creates a user namespace and maps the caller's numeric
UID and primary GID to the same IDs inside it. It then creates a private mount
namespace and mounts a fresh tmpfs as the new root. It maps the host's `/usr`
and loader paths read-only, binds the current procfs and selected sysfs views,
and provides standard devices, fd links, devpts, and shared memory. It creates
fresh tmpfs mounts at `/tmp`, `/run`, `/var/tmp`, and `/home/admin`, writes
minimal passwd/group data, maps common DNS, certificate, loader, and timezone
configuration from `/etc`, and sets a login-like environment. An explicit
operation at one of those destinations replaces that default. After setup it
chroots, drops capabilities, sets `no_new_privs`, and execs the command.

## Test

The smoke tests need a Linux kernel with namespace and mount support. From a
non-Linux development machine, Docker works:

```sh
docker run --rm --privileged -v "$PWD:/work" -w /work debian:trixie-slim \
  sh -lc 'apt-get update &&
          apt-get install -y --no-install-recommends build-essential &&
          make clean &&
          make CFLAGS="-O2 -Wall -Wextra -Werror -std=c11" &&
          ./tests/smoke.sh'
```

## Examples

Open a shell as the default cosmetic `admin` user with a temporary home:

```sh
./microwrap -- /bin/sh
```

Use a different account name and replace its temporary home with a writable
host directory:

```sh
./microwrap \
  --user joe \
  --bind /mnt/joeshome /home/joe \
  -- /bin/sh
```

Use the original empty-root behavior and specify every mapping yourself:

```sh
./microwrap \
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
and mount a fresh procfs:

```sh
./microwrap \
  --no-userns \
  --proc /proc \
  -- /bin/sh -c 'ls /proc/self'
```

## Notes

- `--bind` and `--ro-bind` are non-recursive. Submounts under `SRC` are not
  copied into the wrapper.
- `--ro-bind` remounts the bind target read-only. It is intentionally simple
  and does not use the newer recursive mount-attribute API.
- Rootless mode requires unprivileged user namespaces to be enabled by the
  kernel and distribution policy.
- `--user` is cosmetic. The process keeps the caller's numeric UID and primary
  GID; the name, home, account files, working directory, and environment change.
- Supplementary host groups are not mapped into the user namespace.
- The default `/proc` is a bind of the caller's current procfs, so it shows the
  same PID namespace rather than providing PID isolation. Use `--dir /proc` to
  leave it empty. A fresh `--proc /proc` mount may require `--no-userns` because
  microwrap does not create a PID namespace.
- Selected `/sys` subtrees are bound for CPU, device, and cgroup introspection.
  Use `--tmpfs /sys` or `--no-defaults` to hide them.
- `/dev/tty` and devpts are available, but opening `/dev/tty` still requires the
  caller to have supplied a controlling terminal, such as Docker's `-it` mode.
- Common runtime configuration is mapped read-only from `/etc`, including DNS,
  hosts, NSS network databases, certificates, loader configuration, and
  timezone data. Unrelated files such as host keys and credentials remain
  hidden unless explicitly mapped.
- The writable root and generated `/etc` are private tmpfs state and disappear
  when the wrapped command exits; `/usr` and mapped runtime configuration remain
  read-only. Setuid execution cannot elevate privileges because `no_new_privs`
  is set before exec.
- The process still shares the host PID, IPC, UTS, and network namespaces.
