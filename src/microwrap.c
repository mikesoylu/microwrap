#define _GNU_SOURCE

#ifndef __linux__
#error "microwrap is Linux-only and requires Linux namespace/mount APIs"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/capability.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum op_type {
    OP_BIND,
    OP_RO_BIND,
    OP_TMPFS,
    OP_PROC,
    OP_DIR,
    OP_SYMLINK,
};

struct op {
    enum op_type type;
    const char *a;
    const char *b;
};

enum env_op_type {
    ENV_SET,
    ENV_UNSET,
};

struct env_op {
    enum env_op_type type;
    const char *name;
    const char *value;
};

struct config {
    struct op *ops;
    size_t len;
    size_t cap;
    struct env_op *env_ops;
    size_t env_len;
    size_t env_cap;
    const char *user;
    uid_t uid;
    gid_t gid;
    char *home;
    const char *workdir;
    bool no_userns;
    bool clear_env;
    bool no_defaults;
};

static void die(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "microwrap: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void die_errno(const char *what)
{
    die("%s: %s", what, strerror(errno));
}

static void *xrealloc(void *ptr, size_t size)
{
    void *next = realloc(ptr, size);

    if (!next)
        die_errno("realloc");
    return next;
}

static char *xstrdup(const char *s)
{
    char *copy = strdup(s);

    if (!copy)
        die_errno("strdup");
    return copy;
}

static void add_op(struct config *cfg, enum op_type type, const char *a, const char *b)
{
    if (cfg->len == cfg->cap) {
        cfg->cap = cfg->cap ? cfg->cap * 2 : 16;
        cfg->ops = xrealloc(cfg->ops, cfg->cap * sizeof(cfg->ops[0]));
    }
    cfg->ops[cfg->len++] = (struct op){ .type = type, .a = a, .b = b };
}

static void add_env_op(struct config *cfg, enum env_op_type type,
                       const char *name, const char *value)
{
    if (cfg->env_len == cfg->env_cap) {
        cfg->env_cap = cfg->env_cap ? cfg->env_cap * 2 : 8;
        cfg->env_ops = xrealloc(cfg->env_ops,
                                cfg->env_cap * sizeof(cfg->env_ops[0]));
    }
    cfg->env_ops[cfg->env_len++] = (struct env_op){
        .type = type,
        .name = name,
        .value = value,
    };
}

static bool valid_user_name(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;

    if (!*p || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')
            continue;
        return false;
    }
    return true;
}

static bool valid_env_name(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;

    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_'))
        return false;
    for (p++; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_')
            continue;
        return false;
    }
    return true;
}

static void finalize_config(struct config *cfg)
{
    size_t len;

    if (!valid_user_name(cfg->user))
        die("invalid user name: %s", cfg->user);

    if (!cfg->home) {
        len = strlen("/home/") + strlen(cfg->user) + 1;
        cfg->home = malloc(len);
        if (!cfg->home)
            die_errno("malloc");
        snprintf(cfg->home, len, "/home/%s", cfg->user);
    }

    len = strlen(cfg->home);
    while (len > 1 && cfg->home[len - 1] == '/')
        cfg->home[--len] = '\0';
    if (strcmp(cfg->home, "/") == 0)
        die("--home path must not be /");

    if (!cfg->workdir)
        cfg->workdir = cfg->no_defaults ? "/" : cfg->home;
}

static void usage(FILE *out)
{
    fprintf(out,
        "usage: microwrap [options] -- command [args...]\n"
        "\n"
        "filesystem options:\n"
        "  --bind SRC DST       bind-mount SRC read-write at absolute path DST\n"
        "  --ro-bind SRC DST    bind-mount SRC read-only at absolute path DST\n"
        "  --tmpfs DST          mount an empty tmpfs at absolute path DST\n"
        "  --proc DST           mount procfs at absolute path DST\n"
        "  --dir DST            create a directory at absolute path DST\n"
        "  --symlink TARGET DST create symlink DST -> TARGET inside the wrapper\n"
        "\n"
        "runtime options:\n"
        "  --user NAME          cosmetic account name; defaults to admin\n"
        "  --home DIR           home path; defaults to /home/NAME\n"
        "  --chdir DIR          chdir before exec; defaults to the home path\n"
        "  --setenv NAME VALUE  set an environment variable before exec\n"
        "  --unsetenv NAME      remove an environment variable before exec\n"
        "  --clearenv           clear inherited variables before adding defaults\n"
        "  --no-defaults        disable automatic mounts, account files, and env\n"
        "  --no-userns          skip user namespace setup; requires CAP_SYS_ADMIN\n"
        "  --help               show this help\n");
}

static char **parse_args(int argc, char **argv, struct config *cfg)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        } else if (strcmp(arg, "--bind") == 0) {
            const char *src;
            const char *dst;

            if (i + 2 >= argc)
                die("--bind requires SRC and DST");
            src = argv[++i];
            dst = argv[++i];
            add_op(cfg, OP_BIND, src, dst);
        } else if (strcmp(arg, "--ro-bind") == 0) {
            const char *src;
            const char *dst;

            if (i + 2 >= argc)
                die("--ro-bind requires SRC and DST");
            src = argv[++i];
            dst = argv[++i];
            add_op(cfg, OP_RO_BIND, src, dst);
        } else if (strcmp(arg, "--tmpfs") == 0) {
            if (i + 1 >= argc)
                die("--tmpfs requires DST");
            add_op(cfg, OP_TMPFS, argv[++i], NULL);
        } else if (strcmp(arg, "--proc") == 0) {
            if (i + 1 >= argc)
                die("--proc requires DST");
            add_op(cfg, OP_PROC, argv[++i], NULL);
        } else if (strcmp(arg, "--dir") == 0) {
            if (i + 1 >= argc)
                die("--dir requires DST");
            add_op(cfg, OP_DIR, argv[++i], NULL);
        } else if (strcmp(arg, "--symlink") == 0) {
            const char *target;
            const char *dst;

            if (i + 2 >= argc)
                die("--symlink requires TARGET and DST");
            target = argv[++i];
            dst = argv[++i];
            add_op(cfg, OP_SYMLINK, target, dst);
        } else if (strcmp(arg, "--chdir") == 0) {
            if (i + 1 >= argc)
                die("--chdir requires DIR");
            cfg->workdir = argv[++i];
            if (cfg->workdir[0] != '/')
                die("--chdir path must be absolute: %s", cfg->workdir);
        } else if (strcmp(arg, "--user") == 0) {
            if (i + 1 >= argc)
                die("--user requires NAME");
            cfg->user = argv[++i];
        } else if (strcmp(arg, "--home") == 0) {
            const char *home;

            if (i + 1 >= argc)
                die("--home requires DIR");
            home = argv[++i];
            if (home[0] != '/' || strcmp(home, "/") == 0)
                die("--home path must be absolute and not /: %s", home);
            free(cfg->home);
            cfg->home = xstrdup(home);
        } else if (strcmp(arg, "--setenv") == 0) {
            const char *name;
            const char *value;

            if (i + 2 >= argc)
                die("--setenv requires NAME and VALUE");
            name = argv[++i];
            value = argv[++i];
            if (!valid_env_name(name))
                die("invalid environment variable name: %s", name);
            add_env_op(cfg, ENV_SET, name, value);
        } else if (strcmp(arg, "--unsetenv") == 0) {
            const char *name;

            if (i + 1 >= argc)
                die("--unsetenv requires NAME");
            name = argv[++i];
            if (!valid_env_name(name))
                die("invalid environment variable name: %s", name);
            add_env_op(cfg, ENV_UNSET, name, NULL);
        } else if (strcmp(arg, "--clearenv") == 0) {
            cfg->clear_env = true;
        } else if (strcmp(arg, "--no-defaults") == 0) {
            cfg->no_defaults = true;
        } else if (strcmp(arg, "--no-userns") == 0) {
            cfg->no_userns = true;
        } else if (strcmp(arg, "--help") == 0) {
            usage(stdout);
            exit(0);
        } else {
            die("unknown option: %s", arg);
        }
    }

    if (i >= argc) {
        usage(stderr);
        exit(1);
    }

    return &argv[i];
}

static bool component_is_dotdot(const char *start, size_t len)
{
    return len == 2 && start[0] == '.' && start[1] == '.';
}

static bool component_is_dot(const char *start, size_t len)
{
    return len == 1 && start[0] == '.';
}

static char *join_under_root(const char *root, const char *path)
{
    size_t root_len;
    size_t cap;
    size_t out_len;
    char *out;
    const char *p;

    if (!path || path[0] != '/')
        die("path must be absolute inside wrapper: %s", path ? path : "(null)");

    root_len = strlen(root);
    cap = root_len + strlen(path) + 2;
    out = malloc(cap);
    if (!out)
        die_errno("malloc");

    memcpy(out, root, root_len);
    out_len = root_len;

    p = path;
    while (*p) {
        const char *start;
        size_t len;

        while (*p == '/')
            p++;
        start = p;
        while (*p && *p != '/')
            p++;
        len = (size_t)(p - start);
        if (len == 0 || component_is_dot(start, len))
            continue;
        if (component_is_dotdot(start, len))
            die("path escapes wrapper root: %s", path);

        if (out_len + 1 + len + 1 > cap)
            die("internal path length error");
        out[out_len++] = '/';
        memcpy(out + out_len, start, len);
        out_len += len;
    }

    if (out_len == root_len && root_len > 1 && out[root_len - 1] == '/')
        out_len--;
    out[out_len] = '\0';
    return out;
}

static void mkdir_p(const char *path, mode_t mode)
{
    char *tmp = xstrdup(path);
    char *p;
    struct stat st;

    if (tmp[0] == '\0') {
        free(tmp);
        return;
    }

    for (p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (lstat(tmp, &st) < 0) {
            if (errno != ENOENT)
                die_errno(tmp);
            if (mkdir(tmp, mode) < 0 && errno != EEXIST)
                die_errno(tmp);
        } else if (S_ISLNK(st.st_mode)) {
            die("refusing symlink in wrapper target path: %s", tmp);
        } else if (!S_ISDIR(st.st_mode)) {
            die("not a directory: %s", tmp);
        }
        *p = '/';
    }

    if (lstat(tmp, &st) < 0) {
        if (errno != ENOENT)
            die_errno(tmp);
        if (mkdir(tmp, mode) < 0 && errno != EEXIST)
            die_errno(tmp);
        if (lstat(tmp, &st) < 0)
            die_errno(tmp);
    }
    if (S_ISLNK(st.st_mode))
        die("refusing symlink in wrapper target path: %s", tmp);
    if (!S_ISDIR(st.st_mode))
        die("not a directory: %s", tmp);
    free(tmp);
}

static char *parent_dir(const char *path)
{
    char *copy = xstrdup(path);
    char *slash = strrchr(copy, '/');

    if (!slash) {
        strcpy(copy, ".");
    } else if (slash == copy) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return copy;
}

static void create_empty_file(const char *path)
{
    struct stat st;
    int fd;

    if (lstat(path, &st) == 0) {
        if (S_ISLNK(st.st_mode))
            die("refusing symlink as wrapper target path: %s", path);
        if (!S_ISREG(st.st_mode))
            die("target is not a regular file: %s", path);
        return;
    }
    if (errno != ENOENT)
        die_errno(path);

    fd = open(path, O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        die_errno(path);
    if (close(fd) < 0)
        die_errno(path);
}

static void ensure_bind_target(const char *target, const struct stat *src_st)
{
    char *parent;

    if (S_ISDIR(src_st->st_mode)) {
        mkdir_p(target, 0755);
        return;
    }

    parent = parent_dir(target);
    mkdir_p(parent, 0755);
    free(parent);
    create_empty_file(target);
}

static void mount_bind(const char *src, const char *target, bool readonly)
{
    struct stat st;

    if (stat(src, &st) < 0)
        die_errno(src);
    ensure_bind_target(target, &st);

    if (mount(src, target, NULL, MS_BIND, NULL) < 0)
        die("bind mount %s to %s: %s", src, target, strerror(errno));

    if (readonly && mount(NULL, target, NULL,
                          MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0)
        die("remount %s read-only: %s", target, strerror(errno));
}

static void mount_tmpfs_with_options(const char *target, const char *options)
{
    mkdir_p(target, 0755);
    if (mount("tmpfs", target, "tmpfs", MS_NOSUID | MS_NODEV, options) < 0)
        die_errno("tmpfs mount");
}

static void mount_tmpfs(const char *target)
{
    mount_tmpfs_with_options(target, "mode=755");
}

static void mount_procfs(const char *target)
{
    mkdir_p(target, 0555);
    if (mount("proc", target, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
        die_errno("proc mount");
}

static void make_symlink(const char *target, const char *link_path)
{
    char *parent = parent_dir(link_path);

    mkdir_p(parent, 0755);
    free(parent);
    if (symlink(target, link_path) < 0)
        die_errno(link_path);
}

static void write_all(int fd, const char *path, const char *data)
{
    size_t len = strlen(data);

    while (len > 0) {
        ssize_t written = write(fd, data, len);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            die_errno(path);
        }
        if (written == 0)
            die("short write to %s", path);
        data += written;
        len -= (size_t)written;
    }
}

static void write_file(const char *path, const char *data, bool missing_ok)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);

    if (fd < 0) {
        if (missing_ok && errno == ENOENT)
            return;
        die_errno(path);
    }

    write_all(fd, path, data);
    if (close(fd) < 0)
        die_errno(path);
}

static void write_regular_file(const char *path, const char *data)
{
    char *parent = parent_dir(path);
    int fd;

    mkdir_p(parent, 0755);
    free(parent);
    create_empty_file(path);

    fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        die_errno(path);
    write_all(fd, path, data);
    if (close(fd) < 0)
        die_errno(path);
}

static void write_map(const char *path, unsigned long inside_id, unsigned long outside_id)
{
    char buf[128];

    snprintf(buf, sizeof(buf), "%lu %lu 1\n", inside_id, outside_id);
    write_file(path, buf, false);
}

static void setup_user_namespace(uid_t uid, gid_t gid)
{
    if (unshare(CLONE_NEWUSER) < 0)
        die_errno("unshare(CLONE_NEWUSER)");

    write_file("/proc/self/setgroups", "deny\n", true);
    write_map("/proc/self/uid_map", (unsigned long)uid, (unsigned long)uid);
    write_map("/proc/self/gid_map", (unsigned long)gid, (unsigned long)gid);

    if (setresgid(gid, gid, gid) < 0)
        die_errno("setresgid");
    if (setresuid(uid, uid, uid) < 0)
        die_errno("setresuid");
}

static void setup_namespaces(const struct config *cfg)
{
    if (!cfg->no_userns)
        setup_user_namespace(cfg->uid, cfg->gid);

    if (unshare(CLONE_NEWNS) < 0)
        die_errno("unshare(CLONE_NEWNS)");

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        die_errno("make mounts private");
}

static void drop_caps(void)
{
    struct __user_cap_header_struct header;
    struct __user_cap_data_struct data[2];
    int cap;

    for (cap = 0; cap < 128; cap++) {
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0) {
            if (errno == EINVAL)
                break;
            if (errno != EPERM)
                die_errno("drop capability from bounding set");
        }
    }

    memset(&header, 0, sizeof(header));
    memset(data, 0, sizeof(data));
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;

    if (syscall(SYS_capset, &header, data) < 0)
        die_errno("capset");
}

static void ensure_stdio(void)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (fcntl(i, F_GETFD) >= 0)
            continue;

        int fd = open("/dev/null", i == 0 ? O_RDONLY : O_WRONLY);
        if (fd < 0)
            die_errno("/dev/null");
        if (fd != i) {
            if (dup2(fd, i) < 0)
                die_errno("dup2");
            close(fd);
        }
    }
}

static void close_extra_fds(void)
{
    DIR *dir = opendir("/proc/self/fd");

    if (dir) {
        int keep = dirfd(dir);
        struct dirent *entry;

        while ((entry = readdir(dir))) {
            char *end = NULL;
            long fd;

            errno = 0;
            fd = strtol(entry->d_name, &end, 10);
            if (errno || !end || *end || fd <= 2 || fd == keep)
                continue;
            close((int)fd);
        }
        closedir(dir);
        return;
    }

    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 1024 * 1024)
        max_fd = 1024 * 1024;
    for (long fd = 3; fd < max_fd; fd++)
        close((int)fd);
}

static void apply_op(const char *root, const struct op *op)
{
    char *target;

    switch (op->type) {
    case OP_BIND:
        target = join_under_root(root, op->b);
        mount_bind(op->a, target, false);
        free(target);
        break;
    case OP_RO_BIND:
        target = join_under_root(root, op->b);
        mount_bind(op->a, target, true);
        free(target);
        break;
    case OP_TMPFS:
        target = join_under_root(root, op->a);
        mount_tmpfs(target);
        free(target);
        break;
    case OP_PROC:
        target = join_under_root(root, op->a);
        mount_procfs(target);
        free(target);
        break;
    case OP_DIR:
        target = join_under_root(root, op->a);
        mkdir_p(target, 0755);
        free(target);
        break;
    case OP_SYMLINK:
        target = join_under_root(root, op->b);
        make_symlink(op->a, target);
        free(target);
        break;
    }
}

static const char *op_target(const struct op *op)
{
    switch (op->type) {
    case OP_BIND:
    case OP_RO_BIND:
    case OP_SYMLINK:
        return op->b;
    case OP_TMPFS:
    case OP_PROC:
    case OP_DIR:
        return op->a;
    }
    return NULL;
}

static bool same_path(const char *a, const char *b)
{
    size_t a_len = strlen(a);
    size_t b_len = strlen(b);

    while (a_len > 1 && a[a_len - 1] == '/')
        a_len--;
    while (b_len > 1 && b[b_len - 1] == '/')
        b_len--;
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static bool path_is_below(const char *path, const char *parent)
{
    size_t path_len = strlen(path);
    size_t parent_len = strlen(parent);

    while (path_len > 1 && path[path_len - 1] == '/')
        path_len--;
    while (parent_len > 1 && parent[parent_len - 1] == '/')
        parent_len--;
    if (path_len <= parent_len || memcmp(path, parent, parent_len) != 0)
        return false;
    return parent_len == 1 || path[parent_len] == '/';
}

static bool op_covers_children(const struct op *op)
{
    return op->type != OP_DIR;
}

static bool target_is_overridden(const struct config *cfg, const char *path)
{
    for (size_t i = 0; i < cfg->len; i++) {
        const char *target = op_target(&cfg->ops[i]);

        if (target && (same_path(target, path) ||
                       (op_covers_children(&cfg->ops[i]) &&
                        path_is_below(path, target))))
            return true;
    }
    return false;
}

static char *append_path(const char *base, const char *suffix)
{
    size_t len = strlen(base) + strlen(suffix) + 1;
    char *path = malloc(len);

    if (!path)
        die_errno("malloc");
    snprintf(path, len, "%s%s", base, suffix);
    return path;
}

static char *runtime_dir(const struct config *cfg)
{
    char *path = malloc(64);

    if (!path)
        die_errno("malloc");
    snprintf(path, 64, "/run/user/%lu", (unsigned long)cfg->uid);
    return path;
}

static void mkdir_inside(const char *root, const char *path, mode_t mode)
{
    char *target = join_under_root(root, path);

    mkdir_p(target, mode);
    if (chmod(target, mode) < 0)
        die_errno(target);
    free(target);
}

static void default_dir(const struct config *cfg, const char *root,
                        const char *path, mode_t mode)
{
    if (!target_is_overridden(cfg, path))
        mkdir_inside(root, path, mode);
}

static void default_symlink(const struct config *cfg, const char *root,
                            const char *link_target, const char *path)
{
    char *target;

    if (target_is_overridden(cfg, path))
        return;
    target = join_under_root(root, path);
    make_symlink(link_target, target);
    free(target);
}

static void mirror_host_path(const struct config *cfg, const char *root,
                             const char *path)
{
    char link_target[PATH_MAX + 1];
    struct stat st;
    char *target;
    ssize_t len;

    if (target_is_overridden(cfg, path))
        return;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT)
            return;
        die_errno(path);
    }

    target = join_under_root(root, path);
    if (!S_ISLNK(st.st_mode)) {
        mount_bind(path, target, true);
        free(target);
        return;
    }

    len = readlink(path, link_target, sizeof(link_target) - 1);
    if (len < 0)
        die_errno(path);
    if ((size_t)len == sizeof(link_target) - 1)
        die("symlink target too long: %s", path);
    link_target[len] = '\0';
    make_symlink(link_target, target);
    free(target);
}

static void mirror_resolved_path(const struct config *cfg, const char *root,
                                 const char *path)
{
    struct stat st;
    char *resolved;
    char *target;

    if (target_is_overridden(cfg, path))
        return;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT)
            return;
        die_errno(path);
    }

    mirror_host_path(cfg, root, path);
    if (!S_ISLNK(st.st_mode))
        return;

    resolved = realpath(path, NULL);
    if (!resolved)
        return;
    target = join_under_root(root, resolved);
    mount_bind(path, target, true);
    free(target);
    free(resolved);
}

static void default_bind_path(const struct config *cfg, const char *root,
                              const char *path)
{
    struct stat st;
    char *target;

    if (target_is_overridden(cfg, path))
        return;
    if (stat(path, &st) < 0) {
        if (errno == ENOENT)
            return;
        die_errno(path);
    }

    target = join_under_root(root, path);
    mount_bind(path, target, false);
    free(target);
}

static void default_recursive_bind_path(const struct config *cfg,
                                        const char *root, const char *path)
{
    struct stat st;
    char *target;

    if (target_is_overridden(cfg, path))
        return;
    if (stat(path, &st) < 0) {
        if (errno == ENOENT)
            return;
        die_errno(path);
    }

    target = join_under_root(root, path);
    ensure_bind_target(target, &st);
    if (mount(path, target, NULL, MS_BIND | MS_REC, NULL) < 0)
        die("recursive bind mount %s to %s: %s", path, target, strerror(errno));
    free(target);
}

static void setup_account_files(const struct config *cfg, const char *root)
{
    static const char nsswitch[] =
        "passwd: files\n"
        "group: files\n"
        "hosts: files dns\n";
    size_t passwd_len = strlen(cfg->user) + strlen(cfg->home) + 64;
    size_t group_len = strlen(cfg->user) + 16;
    char *passwd = malloc(passwd_len);
    char *group = malloc(group_len);
    char *target;

    if (!passwd || !group)
        die_errno("malloc");
    snprintf(passwd, passwd_len, "%s:x:%lu:%lu:Microwrap User:%s:/bin/sh\n",
             cfg->user, (unsigned long)cfg->uid, (unsigned long)cfg->gid,
             cfg->home);
    snprintf(group, group_len, "%s:x:%lu:\n", cfg->user,
             (unsigned long)cfg->gid);

    if (!target_is_overridden(cfg, "/etc/passwd")) {
        target = join_under_root(root, "/etc/passwd");
        write_regular_file(target, passwd);
        free(target);
    }
    if (!target_is_overridden(cfg, "/etc/group")) {
        target = join_under_root(root, "/etc/group");
        write_regular_file(target, group);
        free(target);
    }
    if (!target_is_overridden(cfg, "/etc/nsswitch.conf")) {
        target = join_under_root(root, "/etc/nsswitch.conf");
        write_regular_file(target, nsswitch);
        free(target);
    }

    free(group);
    free(passwd);
}

static void setup_default_filesystem(const struct config *cfg, const char *root)
{
    static const char *system_paths[] = {
        "/usr", "/bin", "/sbin", "/lib", "/lib64",
    };
    static const char *device_paths[] = {
        "/dev/null", "/dev/zero", "/dev/full", "/dev/random", "/dev/urandom",
        "/dev/tty", "/dev/pts", "/dev/ptmx",
    };
    static const char *sys_paths[] = {
        "/sys/block", "/sys/bus", "/sys/class", "/sys/dev", "/sys/devices",
        "/sys/fs/cgroup", "/sys/kernel",
    };
    static const char *etc_paths[] = {
        "/etc/hosts",
        "/etc/hostname",
        "/etc/host.conf",
        "/etc/gai.conf",
        "/etc/networks",
        "/etc/protocols",
        "/etc/services",
        "/etc/ssl",
        "/etc/pki",
        "/etc/ca-certificates",
        "/etc/ca-certificates.conf",
        "/etc/alternatives",
        "/etc/ld.so.cache",
        "/etc/ld.so.conf",
        "/etc/ld.so.conf.d",
        "/etc/localtime",
        "/etc/timezone",
        "/etc/os-release",
    };
    static const char *home_dirs[] = {
        "/.cache", "/.config", "/.local/share", "/.local/state",
    };
    char *target;

    for (size_t i = 0; i < sizeof(system_paths) / sizeof(system_paths[0]); i++)
        mirror_host_path(cfg, root, system_paths[i]);
    default_recursive_bind_path(cfg, root, "/proc");
    for (size_t i = 0; i < sizeof(sys_paths) / sizeof(sys_paths[0]); i++)
        default_bind_path(cfg, root, sys_paths[i]);
    for (size_t i = 0; i < sizeof(device_paths) / sizeof(device_paths[0]); i++)
        default_bind_path(cfg, root, device_paths[i]);

    default_symlink(cfg, root, "/proc/self/fd", "/dev/fd");
    default_symlink(cfg, root, "/proc/self/fd/0", "/dev/stdin");
    default_symlink(cfg, root, "/proc/self/fd/1", "/dev/stdout");
    default_symlink(cfg, root, "/proc/self/fd/2", "/dev/stderr");

    if (!target_is_overridden(cfg, "/tmp")) {
        target = join_under_root(root, "/tmp");
        mount_tmpfs_with_options(target, "mode=1777");
        free(target);
    }

    if (!target_is_overridden(cfg, "/run")) {
        char *runtime;

        target = join_under_root(root, "/run");
        mount_tmpfs_with_options(target, "mode=0755");
        free(target);

        default_dir(cfg, root, "/run/lock", 0755);
        default_dir(cfg, root, "/run/user", 0755);
        runtime = runtime_dir(cfg);
        mkdir_inside(root, runtime, 0700);
        free(runtime);
    }

    if (!target_is_overridden(cfg, "/dev/shm")) {
        target = join_under_root(root, "/dev/shm");
        mount_tmpfs_with_options(target, "mode=1777");
        free(target);
    }

    default_dir(cfg, root, "/var", 0755);
    if (!target_is_overridden(cfg, "/var/tmp")) {
        target = join_under_root(root, "/var/tmp");
        mount_tmpfs_with_options(target, "mode=1777");
        free(target);
    }
    default_symlink(cfg, root, "/run", "/var/run");
    default_symlink(cfg, root, "/run/lock", "/var/lock");
    default_dir(cfg, root, "/mnt", 0755);

    if (!target_is_overridden(cfg, cfg->home)) {
        target = join_under_root(root, cfg->home);
        mount_tmpfs_with_options(target, "mode=0700");
        free(target);

        for (size_t i = 0; i < sizeof(home_dirs) / sizeof(home_dirs[0]); i++) {
            char *path = append_path(cfg->home, home_dirs[i]);
            mkdir_inside(root, path, 0700);
            free(path);
        }
    }

    mirror_resolved_path(cfg, root, "/etc/resolv.conf");
    for (size_t i = 0; i < sizeof(etc_paths) / sizeof(etc_paths[0]); i++)
        mirror_host_path(cfg, root, etc_paths[i]);

    setup_account_files(cfg, root);
}

static void set_env(const char *name, const char *value)
{
    if (setenv(name, value, 1) < 0)
        die_errno("setenv");
}

static void set_path_env(const char *name, const char *base, const char *suffix)
{
    char *value = append_path(base, suffix);

    set_env(name, value);
    free(value);
}

static void setup_environment(const struct config *cfg)
{
    char *runtime;
    char *prompt;
    size_t prompt_len;

    if (cfg->clear_env && clearenv() < 0)
        die_errno("clearenv");

    if (!cfg->no_defaults) {
        set_env("HOME", cfg->home);
        set_env("USER", cfg->user);
        set_env("LOGNAME", cfg->user);
        set_env("SHELL", "/bin/sh");
        set_env("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
        set_env("TMPDIR", "/tmp");
        set_path_env("XDG_CACHE_HOME", cfg->home, "/.cache");
        set_path_env("XDG_CONFIG_HOME", cfg->home, "/.config");
        set_path_env("XDG_DATA_HOME", cfg->home, "/.local/share");
        set_path_env("XDG_STATE_HOME", cfg->home, "/.local/state");
        set_path_env("HISTFILE", cfg->home, "/.sh_history");

        runtime = runtime_dir(cfg);
        set_env("XDG_RUNTIME_DIR", runtime);
        free(runtime);

        set_env("LANG", "C.UTF-8");
        set_env("LC_ALL", "C.UTF-8");
        if (!getenv("TERM"))
            set_env("TERM", "xterm");

        prompt_len = strlen(cfg->user) + 3;
        prompt = malloc(prompt_len);
        if (!prompt)
            die_errno("malloc");
        snprintf(prompt, prompt_len, "%s$ ", cfg->user);
        set_env("PS1", prompt);
        free(prompt);
    }

    for (size_t i = 0; i < cfg->env_len; i++) {
        const struct env_op *op = &cfg->env_ops[i];

        if (op->type == ENV_SET) {
            set_env(op->name, op->value);
        } else if (unsetenv(op->name) < 0) {
            die_errno("unsetenv");
        }
    }
}

static void child_main(const struct config *cfg, const char *root, char **cmd)
{
    setup_namespaces(cfg);

    if (mount("tmpfs", root, "tmpfs", MS_NOSUID | MS_NODEV, "mode=755") < 0)
        die_errno("root tmpfs mount");

    if (!cfg->no_defaults)
        setup_default_filesystem(cfg, root);

    for (size_t i = 0; i < cfg->len; i++)
        apply_op(root, &cfg->ops[i]);

    ensure_stdio();
    close_extra_fds();

    if (chroot(root) < 0)
        die_errno("chroot");
    if (chdir("/") < 0)
        die_errno("chdir /");
    if (chdir(cfg->workdir) < 0)
        die_errno("chdir");

    setup_environment(cfg);

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
        die_errno("prctl(PR_SET_NO_NEW_PRIVS)");
    drop_caps();

    execvp(cmd[0], cmd);
    die_errno(cmd[0]);
}

static char *make_temp_root(void)
{
    const char *base = getenv("XDG_RUNTIME_DIR");
    char *template;
    char *root;
    size_t len;

    if (!base || base[0] != '/')
        base = "/tmp";

    len = strlen(base) + strlen("/microwrap.XXXXXX") + 1;
    template = malloc(len);
    if (!template)
        die_errno("malloc");
    snprintf(template, len, "%s/microwrap.XXXXXX", base);

    root = mkdtemp(template);
    if (!root)
        die_errno("mkdtemp");
    return template;
}

int main(int argc, char **argv)
{
    struct config cfg = { .user = "admin" };
    char **cmd;
    char *root;
    pid_t pid;
    int status;

    cfg.uid = getuid();
    cfg.gid = getgid();
    cmd = parse_args(argc, argv, &cfg);
    finalize_config(&cfg);
    root = make_temp_root();
    pid = fork();

    if (pid < 0)
        die_errno("fork");

    if (pid == 0)
        child_main(&cfg, root, cmd);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        die_errno("waitpid");
    }

    if (rmdir(root) < 0)
        fprintf(stderr, "microwrap: warning: failed to remove %s: %s\n", root, strerror(errno));

    free(root);
    free(cfg.home);
    free(cfg.env_ops);
    free(cfg.ops);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}
