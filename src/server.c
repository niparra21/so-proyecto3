#include "bucket.h"
#include "common.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    int fd;
} list_ctx_t;

static void reap_children(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static void list_raw_cb(const char *key, uint64_t size, void *ctx) {
    list_ctx_t *c = ctx;
    send_linef(c->fd, "%s\t%llu\n", key, (unsigned long long)size);
}

static int seen_name(char names[][S3_MAX_KEY], int count, const char *name) {
    int i;
    for (i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    int fd;
    const char *prefix;
    char names[BUCKET_MAX_OBJECTS][S3_MAX_KEY];
    int count;
} list_pretty_ctx_t;

static void list_pretty_cb(const char *key, uint64_t size, void *ctx) {
    list_pretty_ctx_t *c = ctx;
    const char *rest = key + strlen(c->prefix);
    const char *slash = strchr(rest, '/');
    char name[S3_MAX_KEY];
    size_t len;

    if (*rest == '\0') {
        return;
    }
    if (slash) {
        len = (size_t)(slash - rest) + 1;
        if (len >= sizeof(name)) {
            return;
        }
        memcpy(name, rest, len);
        name[len] = '\0';
        if (!seen_name(c->names, c->count, name)) {
            strcpy(c->names[c->count++], name);
            send_linef(c->fd, "PRE %s\n", name);
        }
    } else {
        send_linef(c->fd, "OBJ %s %llu\n", rest, (unsigned long long)size);
    }
}

static void ok(int fd) {
    send_linef(fd, "OK\n");
}

static void err(int fd, const char *msg) {
    send_linef(fd, "ERR %s\n", msg);
}

static void handle_client(int fd) {
    char line[S3_MAX_LINE];
    char cmd[32];

    if (read_line(fd, line, sizeof(line)) <= 0) {
        return;
    }
    cmd[0] = '\0';
    sscanf(line, "%31s", cmd);

    if (strcmp(cmd, "CREATE_BUCKET") == 0) {
        char bucket[S3_MAX_BUCKET];
        if (sscanf(line, "%*s %63s", bucket) != 1) {
            err(fd, "uso: CREATE_BUCKET bucket");
        } else if (bucket_create(bucket) == 0) {
            ok(fd);
        } else {
            err(fd, "no se pudo crear el bucket");
        }
    } else if (strcmp(cmd, "REMOVE_BUCKET") == 0) {
        char bucket[S3_MAX_BUCKET];
        int force;
        if (sscanf(line, "%*s %63s %d", bucket, &force) != 2) {
            err(fd, "uso: REMOVE_BUCKET bucket force");
        } else if (bucket_remove(bucket, force) == 0) {
            ok(fd);
        } else {
            err(fd, "no se pudo eliminar el bucket");
        }
    } else if (strcmp(cmd, "LIST_BUCKETS") == 0) {
        FILE *out;
        send_linef(fd, "OK\n");
        out = fdopen(dup(fd), "w");
        if (out) {
            bucket_list_objects(out);
            fflush(out);
            fclose(out);
        }
        send_linef(fd, ".\n");
    } else if (strcmp(cmd, "LIST") == 0) {
        char bucket[S3_MAX_BUCKET];
        char prefix[S3_MAX_KEY];
        list_pretty_ctx_t ctx;
        prefix[0] = '\0';
        if (sscanf(line, "%*s %63s %255s", bucket, prefix) < 1) {
            err(fd, "uso: LIST bucket [prefix]");
        } else {
            memset(&ctx, 0, sizeof(ctx));
            ctx.fd = fd;
            ctx.prefix = prefix;
            send_linef(fd, "OK\n");
            if (bucket_list_keys(bucket, prefix, list_pretty_cb, &ctx) < 0) {
                err(fd, "no se pudo listar");
            }
            send_linef(fd, ".\n");
        }
    } else if (strcmp(cmd, "LIST_RAW") == 0) {
        char bucket[S3_MAX_BUCKET];
        char prefix[S3_MAX_KEY];
        list_ctx_t ctx;
        prefix[0] = '\0';
        if (sscanf(line, "%*s %63s %255s", bucket, prefix) < 1) {
            err(fd, "uso: LIST_RAW bucket [prefix]");
        } else {
            ctx.fd = fd;
            send_linef(fd, "OK\n");
            bucket_list_keys(bucket, prefix, list_raw_cb, &ctx);
            send_linef(fd, ".\n");
        }
    } else if (strcmp(cmd, "PUT") == 0) {
        char bucket[S3_MAX_BUCKET];
        char key[S3_MAX_KEY];
        unsigned long long size;
        if (sscanf(line, "%*s %63s %255s %llu", bucket, key, &size) != 3) {
            err(fd, "uso: PUT bucket key size");
        } else if (bucket_put_stream(bucket, key, fd, (uint64_t)size) == 0) {
            ok(fd);
        } else {
            err(fd, "no se pudo guardar el objeto");
        }
    } else if (strcmp(cmd, "GET") == 0) {
        char bucket[S3_MAX_BUCKET];
        char key[S3_MAX_KEY];
        if (sscanf(line, "%*s %63s %255s", bucket, key) != 2) {
            err(fd, "uso: GET bucket key");
        } else if (bucket_send_object(bucket, key, fd) < 0) {
            err(fd, "objeto no encontrado");
        }
    } else if (strcmp(cmd, "COPY") == 0) {
        char sb[S3_MAX_BUCKET], sk[S3_MAX_KEY], db[S3_MAX_BUCKET], dk[S3_MAX_KEY];
        if (sscanf(line, "%*s %63s %255s %63s %255s", sb, sk, db, dk) != 4) {
            err(fd, "uso: COPY src_bucket src_key dst_bucket dst_key");
        } else if (bucket_copy_object(sb, sk, db, dk) == 0) {
            ok(fd);
        } else {
            err(fd, "no se pudo copiar");
        }
    } else if (strcmp(cmd, "DELETE") == 0) {
        char bucket[S3_MAX_BUCKET];
        char key[S3_MAX_KEY];
        int recursive;
        if (sscanf(line, "%*s %63s %255s %d", bucket, key, &recursive) != 3) {
            err(fd, "uso: DELETE bucket key recursive");
        } else if ((recursive ? bucket_delete_prefix(bucket, key) : bucket_delete_key(bucket, key)) >= 0) {
            ok(fd);
        } else {
            err(fd, "no se pudo eliminar");
        }
    } else if (strcmp(cmd, "STAT") == 0) {
        char bucket[S3_MAX_BUCKET];
        char key[S3_MAX_KEY];
        uint64_t size;
        if (sscanf(line, "%*s %63s %255s", bucket, key) != 2) {
            err(fd, "uso: STAT bucket key");
        } else if (bucket_stat_key(bucket, key, &size) == 0) {
            send_linef(fd, "OK %llu\n", (unsigned long long)size);
        } else {
            err(fd, "no existe");
        }
    } else {
        err(fd, "comando desconocido");
    }
}

int main(int argc, char **argv) {
    int port = S3_DEFAULT_PORT;
    int server_fd;

    if (argc >= 2) {
        port = atoi(argv[1]);
    }
    if (bucket_init_storage() < 0) {
        perror("buckets");
        return 1;
    }
    signal(SIGCHLD, reap_children);
    server_fd = create_server_socket(port);
    if (server_fd < 0) {
        perror("listen");
        return 1;
    }
    printf("aws-s3_server escuchando en puerto %d\n", port);
    fflush(stdout);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            continue;
        }
        if (fork() == 0) {
            close(server_fd);
            handle_client(client_fd);
            close(client_fd);
            return 0;
        }
        close(client_fd);
    }
}
