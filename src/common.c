#include "common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int parse_s3_uri(const char *text, s3_uri_t *out) {
    const char *p;
    const char *slash;
    size_t bucket_len;

    memset(out, 0, sizeof(*out));
    if (strncmp(text, "s3://", 5) != 0) {
        return 0;
    }

    p = text + 5;
    slash = strchr(p, '/');
    bucket_len = slash ? (size_t)(slash - p) : strlen(p);
    if (bucket_len == 0 || bucket_len >= S3_MAX_BUCKET) {
        return -1;
    }

    memcpy(out->bucket, p, bucket_len);
    out->bucket[bucket_len] = '\0';
    if (slash && slash[1] != '\0') {
        if (strlen(slash + 1) >= S3_MAX_KEY) {
            return -1;
        }
        strcpy(out->key, slash + 1);
        normalize_slashes(out->key);
    }
    out->is_s3 = 1;
    return 1;
}

int connect_to_server(const char *host, int port) {
    int fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int create_server_socket(int port) {
    int fd;
    int yes = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 32) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int read_exact(int fd, void *buf, size_t size) {
    char *p = buf;
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, p + done, size - done);
        if (n == 0) {
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int write_exact(int fd, const void *buf, size_t size) {
    const char *p = buf;
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, p + done, size - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int read_line(int fd, char *buf, size_t size) {
    size_t i = 0;
    while (i + 1 < size) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (c == '\n') {
            break;
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}

int send_linef(int fd, const char *fmt, ...) {
    char line[S3_MAX_LINE];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        return -1;
    }
    return write_exact(fd, line, (size_t)n);
}

int copy_stream(FILE *in, int out_fd, uint64_t size) {
    char buf[8192];
    uint64_t left = size;
    while (left > 0) {
        size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
        size_t got = fread(buf, 1, want, in);
        if (got == 0) {
            return -1;
        }
        if (write_exact(out_fd, buf, got) < 0) {
            return -1;
        }
        left -= got;
    }
    return 0;
}

int recv_to_file(int in_fd, FILE *out, uint64_t size) {
    char buf[8192];
    uint64_t left = size;
    while (left > 0) {
        size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
        if (read_exact(in_fd, buf, want) < 0) {
            return -1;
        }
        if (fwrite(buf, 1, want, out) != want) {
            return -1;
        }
        left -= want;
    }
    return 0;
}

int mkdir_p(const char *path) {
    char tmp[1024];
    char *p;

    if (!path || !*path) {
        return 0;
    }
    if (strlen(path) >= sizeof(tmp)) {
        return -1;
    }
    strcpy(tmp, path);
    normalize_slashes(tmp);

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) < 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int parent_mkdir_p(const char *path) {
    char tmp[1024];
    char *slash;

    if (strlen(path) >= sizeof(tmp)) {
        return -1;
    }
    strcpy(tmp, path);
    normalize_slashes(tmp);
    slash = strrchr(tmp, '/');
    if (!slash) {
        return 0;
    }
    *slash = '\0';
    return mkdir_p(tmp);
}

int path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int path_is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

uint64_t file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return (uint64_t)st.st_size;
}

void normalize_slashes(char *s) {
    for (; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

const char *base_name(const char *path) {
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *p = a > b ? a : b;
    return p ? p + 1 : path;
}

void join_path(char *out, size_t out_size, const char *left, const char *right) {
    size_t len = strlen(left);
    if (len == 0) {
        snprintf(out, out_size, "%s", right);
    } else if (left[len - 1] == '/' || left[len - 1] == '\\') {
        snprintf(out, out_size, "%s%s", left, right);
    } else {
        snprintf(out, out_size, "%s/%s", left, right);
    }
}
