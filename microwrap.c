#define _GNU_SOURCE

#ifndef __linux__
#error "microwrap is Linux-only and requires Linux namespace/mount APIs"
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/if.h>
#include <linux/capability.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
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

enum network_mode {
    NETWORK_HOST,
    NETWORK_NONE,
    NETWORK_INTERNET,
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
    enum network_mode network;
    bool no_userns;
    bool share_pid;
    bool clear_env;
    bool no_defaults;
};

struct text_buffer {
    char *data;
    size_t len;
    size_t cap;
    bool truncated;
};

static void text_add_char(struct text_buffer *buf, char value)
{
    if (buf->len + 1 < buf->cap)
        buf->data[buf->len++] = value;
    else
        buf->truncated = true;
}

static void text_add_string(struct text_buffer *buf, const char *value)
{
    if (!value)
        value = "(null)";
    while (*value)
        text_add_char(buf, *value++);
}

static void text_add_unsigned(struct text_buffer *buf, unsigned long value)
{
    char digits[3 * sizeof(value)];
    size_t len = 0;

    do {
        digits[len++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (len > 0)
        text_add_char(buf, digits[--len]);
}

/* Only the conversions used below are implemented to avoid libc printf code. */
static bool text_vformat(char *data, size_t cap, const char *fmt, va_list ap)
{
    struct text_buffer buf = { .data = data, .cap = cap };

    while (*fmt) {
        bool long_value = false;

        if (*fmt != '%') {
            text_add_char(&buf, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == 'l') {
            long_value = true;
            fmt++;
        }

        switch (*fmt) {
        case 's':
            text_add_string(&buf, va_arg(ap, const char *));
            break;
        case 'd': {
            int value = va_arg(ap, int);
            unsigned int magnitude;

            if (value < 0) {
                text_add_char(&buf, '-');
                magnitude = 0U - (unsigned int)value;
            } else {
                magnitude = (unsigned int)value;
            }
            text_add_unsigned(&buf, magnitude);
            break;
        }
        case 'u':
            text_add_unsigned(&buf, long_value ? va_arg(ap, unsigned long)
                                                : va_arg(ap, unsigned int));
            break;
        case '%':
            text_add_char(&buf, '%');
            break;
        case '\0':
            text_add_char(&buf, '%');
            continue;
        default:
            text_add_char(&buf, '%');
            if (long_value)
                text_add_char(&buf, 'l');
            text_add_char(&buf, *fmt);
            break;
        }
        fmt++;
    }

    if (cap)
        data[buf.len < cap ? buf.len : cap - 1] = '\0';
    return !buf.truncated;
}

static void write_output(int fd, const char *data)
{
    size_t len = strlen(data);

    while (len > 0) {
        ssize_t written = write(fd, data, len);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            _exit(1);
        }
        if (written == 0)
            _exit(1);
        data += written;
        len -= (size_t)written;
    }
}

static _Noreturn void die(const char *fmt, ...)
{
    char message[1024];
    va_list ap;

    va_start(ap, fmt);
    text_vformat(message, sizeof(message), fmt, ap);
    va_end(ap);
    write_output(STDERR_FILENO, "microwrap: ");
    write_output(STDERR_FILENO, message);
    write_output(STDERR_FILENO, "\n");
    _exit(1);
}

static void die_errno(const char *what)
{
    int error = errno;

    die("%s: errno %d", what, error);
}

static void format_text(char *data, size_t cap, const char *fmt, ...)
{
    va_list ap;
    bool complete;

    va_start(ap, fmt);
    complete = text_vformat(data, cap, fmt, ap);
    va_end(ap);
    if (!complete)
        die("internal formatted value is too long");
}

static void warn_remove(const char *path)
{
    char message[1024];
    int error = errno;

    format_text(message, sizeof(message),
                "microwrap: warning: failed to remove %s: errno %d\n",
                path, error);
    write_output(STDERR_FILENO, message);
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
        format_text(cfg->home, len, "/home/%s", cfg->user);
    }

    len = strlen(cfg->home);
    while (len > 1 && cfg->home[len - 1] == '/')
        cfg->home[--len] = '\0';
    if (strcmp(cfg->home, "/") == 0)
        die("--home path must not be /");

    if (!cfg->workdir)
        cfg->workdir = cfg->no_defaults ? "/" : cfg->home;
}

static _Noreturn void show_docs(int fd, int status)
{
    write_output(fd, "https://github.com/mikesoylu/microwrap#usage\n");
    _exit(status);
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
        } else if (strcmp(arg, "--share-pid") == 0) {
            cfg->share_pid = true;
        } else if (strcmp(arg, "--network") == 0) {
            const char *mode;

            if (i + 1 >= argc)
                die("--network requires MODE");
            mode = argv[++i];
            if (strcmp(mode, "host") == 0)
                cfg->network = NETWORK_HOST;
            else if (strcmp(mode, "none") == 0)
                cfg->network = NETWORK_NONE;
            else if (strcmp(mode, "internet") == 0)
                cfg->network = NETWORK_INTERNET;
            else
                die("invalid network mode: %s", mode);
        } else if (strcmp(arg, "--help") == 0) {
            show_docs(STDOUT_FILENO, 0);
        } else {
            die("unknown option: %s", arg);
        }
    }

    if (i >= argc)
        show_docs(STDERR_FILENO, 1);

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
        die("bind mount %s to %s: errno %d", src, target, errno);

    if (readonly && mount(NULL, target, NULL,
                          MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0)
        die("remount %s read-only: errno %d", target, errno);
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

    format_text(buf, sizeof(buf), "%lu %lu 1\n", inside_id, outside_id);
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

struct network_sync {
    int ready_fd;
    int gate_fd;
};

struct blocked_route {
    int family;
    unsigned char prefix_len;
    unsigned char address[16];
};

static void bring_up_loopback(void)
{
    struct ifreq request;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        die_errno("socket for loopback setup");
    memset(&request, 0, sizeof(request));
    memcpy(request.ifr_name, "lo", sizeof("lo"));
    if (ioctl(fd, SIOCGIFFLAGS, &request) < 0)
        die_errno("get loopback flags");
    request.ifr_flags |= IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &request) < 0)
        die_errno("bring up loopback");
    if (close(fd) < 0)
        die_errno("close loopback socket");
}

static void add_unreachable_route(int fd, unsigned int sequence,
                                  const struct blocked_route *route)
{
    struct {
        struct nlmsghdr header;
        struct rtmsg route;
        unsigned char attributes[RTA_SPACE(16)];
    } request;
    struct sockaddr_nl kernel;
    struct rtattr *destination;
    size_t address_len = route->family == AF_INET ? 4 : 16;
    char response[4096];

    memset(&request, 0, sizeof(request));
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.route));
    request.header.nlmsg_type = RTM_NEWROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
                                 NLM_F_CREATE | NLM_F_EXCL;
    request.header.nlmsg_seq = sequence;
    request.route.rtm_family = (unsigned char)route->family;
    request.route.rtm_dst_len = route->prefix_len;
    request.route.rtm_table = RT_TABLE_MAIN;
    request.route.rtm_protocol = RTPROT_STATIC;
    request.route.rtm_scope = RT_SCOPE_UNIVERSE;
    request.route.rtm_type = RTN_UNREACHABLE;

    destination = (struct rtattr *)((char *)&request +
                                    NLMSG_ALIGN(request.header.nlmsg_len));
    destination->rta_type = RTA_DST;
    destination->rta_len = RTA_LENGTH(address_len);
    memcpy(RTA_DATA(destination), route->address, address_len);
    request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) +
                               RTA_LENGTH(address_len);

    memset(&kernel, 0, sizeof(kernel));
    kernel.nl_family = AF_NETLINK;
    if (sendto(fd, &request, request.header.nlmsg_len, 0,
               (struct sockaddr *)&kernel, sizeof(kernel)) < 0)
        die_errno("add isolated-network route");

    for (;;) {
        struct nlmsghdr *header;
        ssize_t len = recv(fd, response, sizeof(response), 0);

        if (len < 0) {
            if (errno == EINTR)
                continue;
            die_errno("acknowledge isolated-network route");
        }
        for (header = (struct nlmsghdr *)response;
             NLMSG_OK(header, (unsigned int)len);
             header = NLMSG_NEXT(header, len)) {
            struct nlmsgerr *error;

            if (header->nlmsg_seq != sequence ||
                header->nlmsg_type != NLMSG_ERROR)
                continue;
            error = NLMSG_DATA(header);
            if (error->error != 0) {
                errno = -error->error;
                die_errno("add isolated-network route");
            }
            return;
        }
    }
}

static void setup_internet_routes(void)
{
    static const struct blocked_route routes[] = {
        { AF_INET, 8,  { 0 } },
        { AF_INET, 8,  { 10 } },
        { AF_INET, 10, { 100, 64 } },
        { AF_INET, 16, { 169, 254 } },
        { AF_INET, 12, { 172, 16 } },
        { AF_INET, 24, { 192, 0, 0 } },
        { AF_INET, 24, { 192, 0, 2 } },
        { AF_INET, 24, { 192, 88, 99 } },
        { AF_INET, 16, { 192, 168 } },
        { AF_INET, 15, { 198, 18 } },
        { AF_INET, 24, { 198, 51, 100 } },
        { AF_INET, 24, { 203, 0, 113 } },
        { AF_INET, 4,  { 224 } },
        { AF_INET, 4,  { 240 } },
    };
    struct sockaddr_nl local;
    int fd;

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        die_errno("open route socket");
    memset(&local, 0, sizeof(local));
    local.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0)
        die_errno("bind route socket");
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        add_unreachable_route(fd, (unsigned int)i + 1, &routes[i]);
    if (close(fd) < 0)
        die_errno("close route socket");
}

static void signal_network_namespace_ready(const struct network_sync *sync)
{
    char byte = '1';

    for (;;) {
        ssize_t written = write(sync->ready_fd, &byte, 1);

        if (written == 1)
            break;
        if (written < 0 && errno == EINTR)
            continue;
        die_errno("signal network namespace readiness");
    }
    if (close(sync->ready_fd) < 0)
        die_errno("close network readiness pipe");

    for (;;) {
        char ignored[16];
        ssize_t len = read(sync->gate_fd, ignored, sizeof(ignored));

        if (len == 0)
            break;
        if (len < 0 && errno == EINTR)
            continue;
        if (len < 0)
            die_errno("wait for Internet helper");
    }
    if (close(sync->gate_fd) < 0)
        die_errno("close network gate pipe");
}

static void setup_namespaces(const struct config *cfg,
                             const struct network_sync *network_sync)
{
    if (!cfg->no_userns)
        setup_user_namespace(cfg->uid, cfg->gid);

    if (unshare(CLONE_NEWNS) < 0)
        die_errno("unshare(CLONE_NEWNS)");

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        die_errno("make mounts private");

    if (!cfg->share_pid && unshare(CLONE_NEWPID) < 0)
        die_errno("unshare(CLONE_NEWPID)");

    if (cfg->network != NETWORK_HOST && unshare(CLONE_NEWNET) < 0)
        die_errno("unshare(CLONE_NEWNET)");
    if (cfg->network == NETWORK_NONE)
        bring_up_loopback();
    if (cfg->network == NETWORK_INTERNET) {
        signal_network_namespace_ready(network_sync);
        setup_internet_routes();
    }
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

static void close_fds_from(unsigned int first)
{
    struct rlimit limit;
    rlim_t max_fd;

#ifdef SYS_close_range
    if (syscall(SYS_close_range, first, ~0U, 0U) == 0)
        return;
#endif

    if (getrlimit(RLIMIT_NOFILE, &limit) < 0)
        max_fd = 1024 * 1024;
    else
        max_fd = limit.rlim_cur;
    if (max_fd > 1024 * 1024)
        max_fd = 1024 * 1024;
    for (rlim_t fd = first; fd < max_fd; fd++)
        close((int)fd);
}

static void close_extra_fds(void)
{
    close_fds_from(3);
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
    format_text(path, len, "%s%s", base, suffix);
    return path;
}

static char *runtime_dir(const struct config *cfg)
{
    char *path = malloc(64);

    if (!path)
        die_errno("malloc");
    format_text(path, 64, "/run/user/%lu", (unsigned long)cfg->uid);
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
        die("recursive bind mount %s to %s: errno %d", path, target, errno);
    free(target);
}

static void remount_bind_readonly(const char *target)
{
    if (mount(NULL, target, NULL,
              MS_BIND | MS_REMOUNT | MS_RDONLY |
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
        die("remount %s read-only: errno %d", target, errno);
}

static void mask_default_path(const struct config *cfg, const char *root,
                              const char *path)
{
    struct stat st;
    char *target;

    if (target_is_overridden(cfg, path))
        return;
    target = join_under_root(root, path);
    if (lstat(target, &st) < 0) {
        if (errno == ENOENT) {
            free(target);
            return;
        }
        die_errno(target);
    }

    if (S_ISDIR(st.st_mode)) {
        if (mount("tmpfs", target, "tmpfs",
                  MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC,
                  "mode=000,size=0") < 0)
            die("mask %s: errno %d", target, errno);
    } else {
        if (mount("/dev/null", target, NULL, MS_BIND, NULL) < 0)
            die("mask %s: errno %d", target, errno);
        remount_bind_readonly(target);
    }
    free(target);
}

static void make_default_path_readonly(const struct config *cfg,
                                       const char *root, const char *path)
{
    struct stat st;
    unsigned long flags = MS_BIND;
    char *target;

    if (target_is_overridden(cfg, path))
        return;
    target = join_under_root(root, path);
    if (lstat(target, &st) < 0) {
        if (errno == ENOENT) {
            free(target);
            return;
        }
        die_errno(target);
    }
    if (S_ISDIR(st.st_mode))
        flags |= MS_REC;
    if (mount(target, target, NULL, flags, NULL) < 0)
        die("bind %s read-only: errno %d", target, errno);
    remount_bind_readonly(target);
    free(target);
}

static void setup_default_procfs(const struct config *cfg, const char *root)
{
    static const char *masked_paths[] = {
        "/proc/acpi",
        "/proc/asound",
        "/proc/interrupts",
        "/proc/kcore",
        "/proc/keys",
        "/proc/latency_stats",
        "/proc/sched_debug",
        "/proc/scsi",
        "/proc/timer_list",
        "/proc/timer_stats",
    };
    static const char *readonly_paths[] = {
        "/proc/sysrq-trigger",
        "/proc/bus",
        "/proc/fs",
        "/proc/irq",
        "/proc/sys",
    };
    char *target;

    if (target_is_overridden(cfg, "/proc"))
        return;
    target = join_under_root(root, "/proc");
    mount_procfs(target);
    free(target);

    for (size_t i = 0; i < sizeof(masked_paths) / sizeof(masked_paths[0]); i++)
        mask_default_path(cfg, root, masked_paths[i]);
    for (size_t i = 0; i < sizeof(readonly_paths) / sizeof(readonly_paths[0]); i++)
        make_default_path_readonly(cfg, root, readonly_paths[i]);
}

static void setup_network_sysfs(const struct config *cfg, const char *root)
{
    static const char *network_paths[] = {
        "/sys/class/net",
        "/sys/devices/virtual/net",
    };
    char *scratch;
    bool needed = false;

    if (cfg->network == NETWORK_HOST)
        return;
    for (size_t i = 0;
         i < sizeof(network_paths) / sizeof(network_paths[0]); i++) {
        if (!target_is_overridden(cfg, network_paths[i]))
            needed = true;
    }
    if (!needed)
        return;

    scratch = join_under_root(root, "/.microwrap-sysfs");
    mkdir_p(scratch, 0755);
    if (mount("sysfs", scratch, "sysfs",
              MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
        die_errno("private sysfs mount");

    for (size_t i = 0;
         i < sizeof(network_paths) / sizeof(network_paths[0]); i++) {
        char *source;
        char *target;

        if (target_is_overridden(cfg, network_paths[i]))
            continue;
        source = append_path(scratch, network_paths[i] + strlen("/sys"));
        target = join_under_root(root, network_paths[i]);
        mount_bind(source, target, true);
        free(target);
        free(source);
    }

    if (umount2(scratch, 0) < 0)
        die_errno("unmount private sysfs staging mount");
    if (rmdir(scratch) < 0)
        die_errno("remove private sysfs staging directory");
    free(scratch);
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
    format_text(passwd, passwd_len,
                "%s:x:%lu:%lu:Microwrap User:%s:/bin/sh\n",
                cfg->user, (unsigned long)cfg->uid, (unsigned long)cfg->gid,
                cfg->home);
    format_text(group, group_len, "%s:x:%lu:\n", cfg->user,
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
    if (cfg->share_pid)
        default_recursive_bind_path(cfg, root, "/proc");
    else
        setup_default_procfs(cfg, root);
    for (size_t i = 0; i < sizeof(sys_paths) / sizeof(sys_paths[0]); i++)
        default_bind_path(cfg, root, sys_paths[i]);
    setup_network_sysfs(cfg, root);
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

    if (cfg->network == NETWORK_INTERNET &&
        !target_is_overridden(cfg, "/etc/resolv.conf")) {
        target = join_under_root(root, "/etc/resolv.conf");
        write_regular_file(target,
                           "nameserver 10.0.2.3\n");
        free(target);
    } else {
        mirror_resolved_path(cfg, root, "/etc/resolv.conf");
    }
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
        format_text(prompt, prompt_len, "%s$ ", cfg->user);
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

struct forwarded_signal {
    int number;
    struct sigaction previous;
};

static struct forwarded_signal forwarded_signals[] = {
    { .number = SIGHUP },
    { .number = SIGINT },
    { .number = SIGQUIT },
    { .number = SIGTERM },
    { .number = SIGALRM },
    { .number = SIGUSR1 },
    { .number = SIGUSR2 },
    { .number = SIGCONT },
    { .number = SIGWINCH },
};
static volatile sig_atomic_t pending_signal;
static sigset_t forwarded_signal_mask;
static sigset_t original_signal_mask;

static void remember_signal(int signal_number)
{
    pending_signal = signal_number;
}

static void install_forward_signal_handlers(void)
{
    struct sigaction action;

    if (sigprocmask(SIG_SETMASK, NULL, &original_signal_mask) < 0)
        die_errno("sigprocmask get");
    sigemptyset(&forwarded_signal_mask);
    memset(&action, 0, sizeof(action));
    action.sa_handler = remember_signal;
    sigemptyset(&action.sa_mask);

    for (size_t i = 0;
         i < sizeof(forwarded_signals) / sizeof(forwarded_signals[0]); i++) {
        sigaddset(&forwarded_signal_mask, forwarded_signals[i].number);
        if (sigaction(forwarded_signals[i].number, &action,
                      &forwarded_signals[i].previous) < 0)
            die_errno("sigaction");
    }
}

static void block_forward_signals(void)
{
    if (sigprocmask(SIG_BLOCK, &forwarded_signal_mask, NULL) < 0)
        die_errno("sigprocmask block");
}

static void restore_signal_mask(void)
{
    if (sigprocmask(SIG_SETMASK, &original_signal_mask, NULL) < 0)
        die_errno("sigprocmask restore");
}

static void restore_forward_signal_handlers(void)
{
    for (size_t i = 0;
         i < sizeof(forwarded_signals) / sizeof(forwarded_signals[0]); i++) {
        if (sigaction(forwarded_signals[i].number,
                      &forwarded_signals[i].previous, NULL) < 0)
            die_errno("sigaction restore");
    }
}

static int wait_status_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

static pid_t fork_process(void)
{
    /* Microwrap is single-threaded, so libc's fork coordination is unnecessary. */
    return (pid_t)syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
}

static void forward_pending_signal(pid_t child)
{
    int signal_number = pending_signal;

    if (!signal_number)
        return;
    pending_signal = 0;
    if (kill(child, signal_number) < 0 && errno != ESRCH)
        die("forward signal %d: errno %d", signal_number, errno);
}

static int supervise_process(pid_t child, bool reap_orphans)
{
    int status;

    for (;;) {
        pid_t waited;

        forward_pending_signal(child);
        waited = waitpid(reap_orphans ? -1 : child, &status, 0);
        if (waited < 0) {
            if (errno == EINTR)
                continue;
            die_errno("waitpid");
        }
        if (waited == child)
            return wait_status_code(status);
    }
}

static void setup_sandbox(const struct config *cfg, const char *root)
{
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
}

static void exec_command(char **cmd)
{
    restore_forward_signal_handlers();
    restore_signal_mask();

    execvp(cmd[0], cmd);
    die_errno(cmd[0]);
}

static int pid_namespace_main(const struct config *cfg, const char *root,
                              char **cmd)
{
    pid_t pid;

    setup_sandbox(cfg, root);
    pid = fork_process();
    if (pid < 0)
        die_errno("fork command");
    if (pid == 0)
        exec_command(cmd);
    restore_signal_mask();
    return supervise_process(pid, true);
}

static int child_main(const struct config *cfg, const char *root, char **cmd,
                      const struct network_sync *network_sync)
{
    pid_t pid;

    setup_namespaces(cfg, network_sync);
    if (cfg->share_pid) {
        setup_sandbox(cfg, root);
        exec_command(cmd);
    }

    pid = fork_process();
    if (pid < 0)
        die_errno("fork PID namespace init");
    if (pid == 0) {
        pending_signal = 0;
        _exit(pid_namespace_main(cfg, root, cmd));
    }
    restore_signal_mask();
    return supervise_process(pid, false);
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
    format_text(template, len, "%s/microwrap.XXXXXX", base);

    root = mkdtemp(template);
    if (!root)
        die_errno("mkdtemp");
    return template;
}

struct network_helper {
    pid_t pid;
    int exit_fd;
};

static void make_cloexec_pipe(int fds[2])
{
    if (pipe2(fds, O_CLOEXEC) < 0)
        die_errno("pipe2");
}

static bool read_readiness_byte(int fd, pid_t sandbox)
{
    char byte;

    for (;;) {
        ssize_t len = read(fd, &byte, 1);

        if (len == 1)
            return true;
        if (len == 0)
            return false;
        if (errno == EINTR) {
            forward_pending_signal(sandbox);
            continue;
        }
        die_errno("read network readiness pipe");
    }
}

static int wait_for_process(pid_t pid)
{
    int status;

    for (;;) {
        pid_t waited = waitpid(pid, &status, 0);

        if (waited == pid)
            return wait_status_code(status);
        if (waited < 0 && errno == EINTR)
            continue;
        die_errno("waitpid");
    }
}

static void prepare_helper_fds(int ready_fd, int exit_fd)
{
    int saved_ready;
    int saved_exit;

    saved_ready = fcntl(ready_fd, F_DUPFD_CLOEXEC, 5);
    if (saved_ready < 0)
        die_errno("duplicate slirp4netns ready fd");
    saved_exit = fcntl(exit_fd, F_DUPFD_CLOEXEC, 5);
    if (saved_exit < 0)
        die_errno("duplicate slirp4netns exit fd");
    if (dup2(saved_ready, 3) < 0)
        die_errno("install slirp4netns ready fd");
    if (dup2(saved_exit, 4) < 0)
        die_errno("install slirp4netns exit fd");
    close_fds_from(5);
}

static void silence_network_helper(void)
{
    int fd = open("/dev/null", O_WRONLY | O_CLOEXEC);

    if (fd < 0)
        die_errno("open /dev/null for slirp4netns");
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0)
        die_errno("redirect slirp4netns output");
    close(fd);
}

static bool launch_network_helper(pid_t sandbox, struct network_helper *helper)
{
    int ready_pipe[2];
    int exit_pipe[2];
    pid_t pid;

    make_cloexec_pipe(ready_pipe);
    make_cloexec_pipe(exit_pipe);
    pid = fork_process();
    if (pid < 0)
        die_errno("fork slirp4netns");
    if (pid == 0) {
        char target[64];
        char *args[] = {
            (char *)"slirp4netns",
            (char *)"--configure",
            (char *)"--disable-host-loopback",
            (char *)"--ready-fd=3",
            (char *)"--exit-fd=4",
            target,
            (char *)"tap0",
            NULL,
        };

        prepare_helper_fds(ready_pipe[1], exit_pipe[0]);
        restore_forward_signal_handlers();
        restore_signal_mask();
        format_text(target, sizeof(target), "%lu", (unsigned long)sandbox);
        silence_network_helper();
        execvp(args[0], args);
        die_errno("slirp4netns");
    }

    close(ready_pipe[1]);
    close(exit_pipe[0]);
    helper->pid = pid;
    helper->exit_fd = exit_pipe[1];
    if (!read_readiness_byte(ready_pipe[0], sandbox)) {
        close(ready_pipe[0]);
        close(helper->exit_fd);
        wait_for_process(pid);
        helper->pid = -1;
        helper->exit_fd = -1;
        write_output(STDERR_FILENO,
                     "microwrap: slirp4netns exited before the network was ready\n");
        return false;
    }
    close(ready_pipe[0]);
    return true;
}

static void kill_blocked_sandbox(pid_t sandbox)
{
    if (kill(sandbox, SIGKILL) < 0 && errno != ESRCH)
        die_errno("kill sandbox after network setup failure");
    wait_for_process(sandbox);
}

static int supervise_networked_process(pid_t sandbox,
                                       struct network_helper *helper)
{
    int status;

    for (;;) {
        pid_t waited;

        forward_pending_signal(sandbox);
        waited = waitpid(-1, &status, 0);
        if (waited < 0) {
            if (errno == EINTR)
                continue;
            die_errno("waitpid");
        }
        if (waited == sandbox) {
            int result = wait_status_code(status);

            close(helper->exit_fd);
            wait_for_process(helper->pid);
            return result;
        }
        if (waited == helper->pid) {
            pid_t child_waited;

            close(helper->exit_fd);
            child_waited = waitpid(sandbox, &status, WNOHANG);
            if (child_waited == sandbox)
                return wait_status_code(status);
            if (child_waited < 0 && errno != EINTR)
                die_errno("waitpid");

            write_output(STDERR_FILENO,
                         "microwrap: slirp4netns exited unexpectedly\n");
            if (kill(sandbox, SIGTERM) < 0 && errno != ESRCH)
                die_errno("terminate sandbox after slirp4netns exit");
            supervise_process(sandbox, false);
            return 1;
        }
    }
}

int main(int argc, char **argv)
{
    struct config cfg = { .user = "admin" };
    struct network_helper network_helper = { .pid = -1, .exit_fd = -1 };
    struct network_sync child_network_sync = { .ready_fd = -1, .gate_fd = -1 };
    int network_ready_pipe[2] = { -1, -1 };
    int network_gate_pipe[2] = { -1, -1 };
    char **cmd;
    char *root;
    pid_t pid;
    int status;

    cfg.uid = getuid();
    cfg.gid = getgid();
    cmd = parse_args(argc, argv, &cfg);
    finalize_config(&cfg);
    root = make_temp_root();
    if (cfg.network == NETWORK_INTERNET) {
        make_cloexec_pipe(network_ready_pipe);
        make_cloexec_pipe(network_gate_pipe);
    }
    install_forward_signal_handlers();
    pid = fork_process();

    if (pid < 0)
        die_errno("fork");

    if (pid == 0) {
        pending_signal = 0;
        block_forward_signals();
        if (cfg.network == NETWORK_INTERNET) {
            close(network_ready_pipe[0]);
            close(network_gate_pipe[1]);
            child_network_sync.ready_fd = network_ready_pipe[1];
            child_network_sync.gate_fd = network_gate_pipe[0];
        }
        _exit(child_main(&cfg, root, cmd,
                         cfg.network == NETWORK_INTERNET
                             ? &child_network_sync : NULL));
    }

    if (cfg.network == NETWORK_INTERNET) {
        bool namespace_ready;

        close(network_ready_pipe[1]);
        close(network_gate_pipe[0]);
        namespace_ready = read_readiness_byte(network_ready_pipe[0], pid);
        close(network_ready_pipe[0]);
        if (!namespace_ready) {
            close(network_gate_pipe[1]);
            status = wait_for_process(pid);
        } else if (!launch_network_helper(pid, &network_helper)) {
            kill_blocked_sandbox(pid);
            close(network_gate_pipe[1]);
            status = 1;
        } else {
            close(network_gate_pipe[1]);
            status = supervise_networked_process(pid, &network_helper);
        }
    } else {
        status = supervise_process(pid, false);
    }
    restore_forward_signal_handlers();

    if (rmdir(root) < 0)
        warn_remove(root);

    free(root);
    free(cfg.home);
    free(cfg.env_ops);
    free(cfg.ops);

    return status;
}
