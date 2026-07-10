#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void fail(const char *message)
{
    fprintf(stderr, "test-network: %s: %s\n", message, strerror(errno));
    exit(1);
}

static unsigned short parse_port(const char *text)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || !*text || *end || value > 65535) {
        fprintf(stderr, "test-network: invalid port: %s\n", text);
        exit(2);
    }
    return (unsigned short)value;
}

static int make_listener(const char *address, const char *port_text)
{
    struct sockaddr_in socket_address;
    int fd;
    int reuse = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        fail("socket");
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
        fail("setsockopt");
    memset(&socket_address, 0, sizeof(socket_address));
    socket_address.sin_family = AF_INET;
    socket_address.sin_port = htons(parse_port(port_text));
    if (inet_pton(AF_INET, address, &socket_address.sin_addr) != 1) {
        fprintf(stderr, "test-network: invalid IPv4 address: %s\n", address);
        exit(2);
    }
    if (bind(fd, (struct sockaddr *)&socket_address, sizeof(socket_address)) < 0)
        fail("bind");
    if (listen(fd, 8) < 0)
        fail("listen");
    return fd;
}

static void write_ready_file(const char *path, int listener)
{
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    char port[32];
    int port_len;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
        fail("open ready file");
    if (getsockname(listener, (struct sockaddr *)&address, &address_len) < 0)
        fail("getsockname");
    port_len = snprintf(port, sizeof(port), "%u\n", ntohs(address.sin_port));
    if (port_len < 0 || (size_t)port_len >= sizeof(port)) {
        errno = EOVERFLOW;
        fail("format listener port");
    }
    if (write(fd, port, (size_t)port_len) != (ssize_t)port_len)
        fail("write ready file");
    if (close(fd) < 0)
        fail("close ready file");
}

static int connect_to(const char *host, const char *service)
{
    struct addrinfo hints;
    struct addrinfo *addresses;
    struct addrinfo *address;
    int result;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    result = getaddrinfo(host, service, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "test-network: resolve %s: %s\n",
                host, gai_strerror(result));
        return 1;
    }

    alarm(10);
    for (address = addresses; address; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype,
                        address->ai_protocol);

        if (fd < 0)
            continue;
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            close(fd);
            alarm(0);
            freeaddrinfo(addresses);
            return 0;
        }
        close(fd);
    }
    alarm(0);
    freeaddrinfo(addresses);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "bind") == 0) {
        int fd = make_listener(argv[2], argv[3]);

        close(fd);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "hold") == 0) {
        int fd = make_listener(argv[2], argv[3]);

        write_ready_file(argv[4], fd);
        for (;;)
            pause();
        close(fd);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "connect") == 0)
        return connect_to(argv[2], argv[3]);

    fprintf(stderr,
            "usage: test-network bind ADDRESS PORT | "
            "hold ADDRESS PORT READY_FILE | connect HOST PORT\n");
    return 2;
}
