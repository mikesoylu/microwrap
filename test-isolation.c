#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/keyctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define KEY_DESCRIPTION "microwrap-test"
#define X32_SYSCALL_BIT 0x40000000L

static _Noreturn void fail(const char *message)
{
    fprintf(stderr, "test-isolation: %s: %s\n", message, strerror(errno));
    exit(2);
}

static const char *errno_name(int error)
{
    static char other[32];

    switch (error) {
    case EPERM: return "EPERM";
    case ENOTTY: return "ENOTTY";
    case ENOSYS: return "ENOSYS";
    case EINVAL: return "EINVAL";
    case EIO: return "EIO";
    case EBADF: return "EBADF";
    }
    snprintf(other, sizeof(other), "errno %d", error);
    return other;
}

/* Issue a terminal ioctl on stdin and print the resulting errno name. */
static int try_ioctl(const char *name)
{
    char byte = '#';
    long result;

    errno = 0;
    if (strcmp(name, "tiocsti") == 0) {
        result = syscall(SYS_ioctl, 0, (unsigned long)TIOCSTI, &byte);
    } else if (strcmp(name, "tiocsti-high") == 0) {
#if ULONG_MAX > 0xffffffffUL
        /* The kernel truncates the request to 32 bits. */
        result = syscall(SYS_ioctl, 0, (1UL << 32) | TIOCSTI, &byte);
#else
        puts("unsupported");
        return 0;
#endif
    } else if (strcmp(name, "tioclinux") == 0) {
        result = syscall(SYS_ioctl, 0, (unsigned long)TIOCLINUX, &byte);
    } else if (strcmp(name, "tiocgwinsz") == 0) {
        struct winsize size;

        result = syscall(SYS_ioctl, 0, (unsigned long)TIOCGWINSZ, &size);
    } else if (strcmp(name, "tiocsti-i386") == 0) {
#if defined(__x86_64__)
        /* int $0x80 enters the i386 syscall ABI; 54 is its ioctl. */
        long ret;

        __asm__ volatile("int $0x80"
                         : "=a"(ret)
                         : "a"(54L), "b"(0L), "c"((long)TIOCSTI), "d"(0L)
                         : "r8", "r9", "r10", "r11", "memory");
        if (ret < 0) {
            errno = (int)-ret;
            result = -1;
        } else {
            result = ret;
        }
#else
        puts("unsupported");
        return 0;
#endif
    } else if (strcmp(name, "tiocsti-x32") == 0) {
#if defined(__x86_64__)
        result = syscall(X32_SYSCALL_BIT | 514, 0, (unsigned long)TIOCSTI, &byte);
#else
        puts("unsupported");
        return 0;
#endif
    } else if (strcmp(name, "tiocsti-x32-64") == 0) {
#if defined(__x86_64__)
        result = syscall(X32_SYSCALL_BIT | 16, 0, (unsigned long)TIOCSTI, &byte);
#else
        puts("unsupported");
        return 0;
#endif
    } else {
        fprintf(stderr, "test-isolation: unknown ioctl: %s\n", name);
        return 2;
    }

    puts(result == 0 ? "OK" : errno_name(errno));
    return 0;
}

static int open_pty(char **name)
{
    int fd = posix_openpt(O_RDWR | O_NOCTTY);

    if (fd < 0)
        fail("posix_openpt");
    if (grantpt(fd) < 0)
        fail("grantpt");
    if (unlockpt(fd) < 0)
        fail("unlockpt");
    *name = ptsname(fd);
    if (!*name)
        fail("ptsname");
    return fd;
}

static int pty_command(int argc, char **argv)
{
    struct stat st;
    char *name;
    int fd;

    if (argc == 3 && strcmp(argv[2], "open") == 0) {
        fd = open_pty(&name);
        if (stat(name, &st) < 0)
            fail(name);
        puts(name);
        close(fd);
        return 0;
    }
    if (argc == 4 && strcmp(argv[2], "hold") == 0) {
        FILE *ready;

        fd = open_pty(&name);
        ready = fopen(argv[3], "w");
        if (!ready || fprintf(ready, "%s\n", name) < 0 || fclose(ready) != 0)
            fail("write ready file");
        for (;;)
            pause();
    }
    fprintf(stderr, "usage: test-isolation pty open | pty hold READY_FILE\n");
    return 2;
}

static int shm_command(int argc, char **argv)
{
    key_t key;
    int id;

    if (argc != 4) {
        fprintf(stderr, "usage: test-isolation shm create|exists|remove KEY\n");
        return 2;
    }
    key = (key_t)strtol(argv[3], NULL, 10);
    if (strcmp(argv[2], "create") == 0) {
        if (shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600) < 0)
            fail("shmget create");
        return 0;
    }
    if (strcmp(argv[2], "exists") == 0) {
        if (shmget(key, 0, 0) >= 0)
            return 0;
        if (errno == ENOENT)
            return 1;
        fail("shmget");
    }
    if (strcmp(argv[2], "remove") == 0) {
        id = shmget(key, 0, 0);
        if (id < 0 || shmctl(id, IPC_RMID, NULL) < 0)
            fail("shm remove");
        return 0;
    }
    fprintf(stderr, "test-isolation: unknown shm action: %s\n", argv[2]);
    return 2;
}

static int keyring_command(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[2], "exec") == 0) {
        /* Start a fresh session keyring holding a secret, then run argv[3...]. */
        if (syscall(SYS_keyctl, KEYCTL_JOIN_SESSION_KEYRING, NULL) < 0)
            fail("join session keyring");
        if (syscall(SYS_add_key, "user", KEY_DESCRIPTION, "secret", 6,
                    KEY_SPEC_SESSION_KEYRING) < 0)
            fail("add_key");
        execvp(argv[3], &argv[3]);
        fail(argv[3]);
    }
    if (argc == 3 && strcmp(argv[2], "check") == 0) {
        /* Exit 0 when the secret is reachable, 1 when it is not. */
        if (syscall(SYS_keyctl, KEYCTL_SEARCH, KEY_SPEC_SESSION_KEYRING,
                    "user", KEY_DESCRIPTION, 0) >= 0)
            return 0;
        if (errno == ENOKEY)
            return 1;
        fail("keyctl search");
    }
    fprintf(stderr, "usage: test-isolation keyring exec CMD... | keyring check\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "ioctl") == 0)
        return try_ioctl(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "pty") == 0)
        return pty_command(argc, argv);
    if (argc >= 3 && strcmp(argv[1], "shm") == 0)
        return shm_command(argc, argv);
    if (argc >= 3 && strcmp(argv[1], "keyring") == 0)
        return keyring_command(argc, argv);

    fprintf(stderr,
            "usage: test-isolation ioctl NAME | pty ... | shm ... | keyring ...\n");
    return 2;
}
