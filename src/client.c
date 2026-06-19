#include "common.h"
#include "bucket.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char key[S3_MAX_KEY];
    uint64_t size;
} remote_item_t;

typedef struct {
    remote_item_t items[BUCKET_MAX_OBJECTS];
    int count;
} remote_list_t;

static const char *host = S3_DEFAULT_HOST;
static int port = S3_DEFAULT_PORT;

static int open_conn(void) {
    int fd = connect_to_server(host, port);
    if (fd < 0) {
        fprintf(stderr, "No se pudo conectar a %s:%d\n", host, port);
    }
    return fd;
}

static int expect_ok(int fd) {
    char line[S3_MAX_LINE];
    if (read_line(fd, line, sizeof(line)) < 0) {
        return -1;
    }
    if (strncmp(line, "OK", 2) == 0) {
        return 0;
    }
    fprintf(stderr, "%s\n", line);
    return -1;
}

static int simple_cmd(const char *line) {
    int fd = open_conn();
    int rc;
    if (fd < 0) {
        return 1;
    }
    if (send_linef(fd, "%s\n", line) < 0) {
        close(fd);
        return 1;
    }
    rc = expect_ok(fd);
    close(fd);
    return rc == 0 ? 0 : 1;
}

static int send_create_bucket(const char *bucket) {
    char line[S3_MAX_LINE];
    snprintf(line, sizeof(line), "CREATE_BUCKET %s", bucket);
    return simple_cmd(line);
}

static int send_remove_bucket(const char *bucket, int force) {
    char line[S3_MAX_LINE];
    snprintf(line, sizeof(line), "REMOVE_BUCKET %s %d", bucket, force);
    return simple_cmd(line);
}

static int list_until_dot(int fd) {
    char line[S3_MAX_LINE];
    if (expect_ok(fd) < 0) {
        return 1;
    }
    while (read_line(fd, line, sizeof(line)) >= 0) {
        if (strcmp(line, ".") == 0) {
            return 0;
        }
        puts(line);
    }
    return 1;
}

static int cmd_ls(int argc, char **argv) {
    int fd;
    s3_uri_t u;

    fd = open_conn();
    if (fd < 0) {
        return 1;
    }
    if (argc == 0) {
        send_linef(fd, "LIST_BUCKETS\n");
    } else if (parse_s3_uri(argv[0], &u) == 1) {
        send_linef(fd, "LIST %s %s\n", u.bucket, u.key);
    } else {
        fprintf(stderr, "Uso: aws-s3 ls [s3://bucket/prefix]\n");
        close(fd);
        return 1;
    }
    return list_until_dot(fd);
}

static int put_file(const char *local, const char *bucket, const char *key) {
    FILE *in;
    int fd;
    uint64_t size;
    char line[S3_MAX_LINE];
    int rc;

    in = fopen(local, "rb");
    if (!in) {
        perror(local);
        return 1;
    }
    fd = open_conn();
    if (fd < 0) {
        fclose(in);
        return 1;
    }
    size = file_size(local);
    snprintf(line, sizeof(line), "PUT %s %s %llu\n", bucket, key, (unsigned long long)size);
    if (write_exact(fd, line, strlen(line)) < 0 || copy_stream(in, fd, size) < 0) {
        fclose(in);
        close(fd);
        return 1;
    }
    fclose(in);
    rc = expect_ok(fd);
    close(fd);
    return rc == 0 ? 0 : 1;
}

static int get_file(const char *bucket, const char *key, const char *local) {
    int fd;
    char line[S3_MAX_LINE];
    unsigned long long size;
    FILE *out;

    fd = open_conn();
    if (fd < 0) {
        return 1;
    }
    send_linef(fd, "GET %s %s\n", bucket, key);
    if (read_line(fd, line, sizeof(line)) < 0 || sscanf(line, "OK %llu", &size) != 1) {
        fprintf(stderr, "%s\n", line);
        close(fd);
        return 1;
    }
    if (parent_mkdir_p(local) < 0) {
        close(fd);
        return 1;
    }
    out = fopen(local, "wb");
    if (!out) {
        perror(local);
        close(fd);
        return 1;
    }
    if (recv_to_file(fd, out, (uint64_t)size) < 0) {
        fclose(out);
        close(fd);
        return 1;
    }
    fclose(out);
    close(fd);
    return 0;
}

static int copy_remote(const char *sb, const char *sk, const char *db, const char *dk) {
    char line[S3_MAX_LINE];
    snprintf(line, sizeof(line), "COPY %s %s %s %s", sb, sk, db, dk);
    return simple_cmd(line);
}

static int delete_remote(const char *bucket, const char *key, int recursive) {
    char line[S3_MAX_LINE];
    snprintf(line, sizeof(line), "DELETE %s %s %d", bucket, key, recursive);
    return simple_cmd(line);
}

static void key_join(char *out, size_t size, const char *prefix, const char *name) {
    if (!prefix || !*prefix) {
        snprintf(out, size, "%s", name);
    } else if (prefix[strlen(prefix) - 1] == '/') {
        snprintf(out, size, "%s%s", prefix, name);
    } else {
        snprintf(out, size, "%s/%s", prefix, name);
    }
}

static int upload_tree(const char *root, const char *path, const char *bucket, const char *prefix) {
    DIR *dir;
    struct dirent *de;
    char current[1024];
    char key[S3_MAX_KEY];
    size_t root_len = strlen(root);

    if (path_is_file(path)) {
        const char *rel = path + root_len;
        if (*rel == '/' || *rel == '\\') {
            rel++;
        }
        snprintf(key, sizeof(key), "%s", rel);
        normalize_slashes(key);
        if (prefix && *prefix) {
            char tmp[S3_MAX_KEY];
            key_join(tmp, sizeof(tmp), prefix, key);
            strcpy(key, tmp);
        }
        printf("upload: %s -> s3://%s/%s\n", path, bucket, key);
        return put_file(path, bucket, key);
    }

    dir = opendir(path);
    if (!dir) {
        perror(path);
        return 1;
    }
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        join_path(current, sizeof(current), path, de->d_name);
        if (path_is_dir(current)) {
            if (upload_tree(root, current, bucket, prefix) != 0) {
                closedir(dir);
                return 1;
            }
        } else if (path_is_file(current)) {
            if (upload_tree(root, current, bucket, prefix) != 0) {
                closedir(dir);
                return 1;
            }
        }
    }
    closedir(dir);
    return 0;
}

static int fetch_remote_list(const char *bucket, const char *prefix, remote_list_t *out) {
    int fd = open_conn();
    char line[S3_MAX_LINE];

    memset(out, 0, sizeof(*out));
    if (fd < 0) {
        return 1;
    }
    send_linef(fd, "LIST_RAW %s %s\n", bucket, prefix ? prefix : "");
    if (expect_ok(fd) < 0) {
        close(fd);
        return 1;
    }
    while (read_line(fd, line, sizeof(line)) >= 0) {
        char *tab;
        if (strcmp(line, ".") == 0) {
            close(fd);
            return 0;
        }
        if (out->count >= BUCKET_MAX_OBJECTS) {
            continue;
        }
        tab = strchr(line, '\t');
        if (!tab) {
            continue;
        }
        *tab = '\0';
        strncpy(out->items[out->count].key, line, S3_MAX_KEY - 1);
        out->items[out->count].size = strtoull(tab + 1, NULL, 10);
        out->count++;
    }
    close(fd);
    return 1;
}

static int remote_index(remote_list_t *list, const char *key) {
    int i;
    for (i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int copy_remote_prefix_to_local(const char *bucket, const char *prefix, const char *dest) {
    remote_list_t remote;
    int i;

    if (fetch_remote_list(bucket, prefix, &remote) != 0) {
        return 1;
    }
    for (i = 0; i < remote.count; i++) {
        char local[1024];
        const char *rel = remote.items[i].key + strlen(prefix);
        if (*rel == '/') {
            rel++;
        }
        join_path(local, sizeof(local), dest, rel);
        printf("download: s3://%s/%s -> %s\n", bucket, remote.items[i].key, local);
        if (get_file(bucket, remote.items[i].key, local) != 0) {
            return 1;
        }
    }
    return 0;
}

static int copy_remote_prefix_to_remote(const char *sb, const char *sprefix,
                                        const char *db, const char *dprefix) {
    remote_list_t remote;
    int i;

    if (fetch_remote_list(sb, sprefix, &remote) != 0) {
        return 1;
    }
    for (i = 0; i < remote.count; i++) {
        char dst_key[S3_MAX_KEY];
        const char *rel = remote.items[i].key + strlen(sprefix);
        if (*rel == '/') {
            rel++;
        }
        key_join(dst_key, sizeof(dst_key), dprefix, rel);
        printf("copy: s3://%s/%s -> s3://%s/%s\n", sb, remote.items[i].key, db, dst_key);
        if (copy_remote(sb, remote.items[i].key, db, dst_key) != 0) {
            return 1;
        }
    }
    return 0;
}

static int cmd_cp(int argc, char **argv) {
    s3_uri_t src, dst;
    int src_s3;
    int dst_s3;
    int recursive = 0;
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--recursive") == 0) {
            recursive = 1;
            for (; i + 1 < argc; i++) {
                argv[i] = argv[i + 1];
            }
            argc--;
            break;
        }
    }
    if (argc != 2) {
        fprintf(stderr, "Uso: aws-s3 cp origen destino [--recursive]\n");
        return 1;
    }
    src_s3 = parse_s3_uri(argv[0], &src);
    dst_s3 = parse_s3_uri(argv[1], &dst);
    if (src_s3 < 0 || dst_s3 < 0 || (!src_s3 && !dst_s3)) {
        fprintf(stderr, "Debe copiar entre una ruta local y S3, o entre dos rutas S3\n");
        return 1;
    }
    if (!src_s3 && dst_s3) {
        if (path_is_dir(argv[0])) {
            if (!recursive) {
                fprintf(stderr, "Use --recursive para copiar directorios\n");
                return 1;
            }
            return upload_tree(argv[0], argv[0], dst.bucket, dst.key);
        }
        if (dst.key[0] == '\0' || dst.key[strlen(dst.key) - 1] == '/') {
            char key[S3_MAX_KEY];
            key_join(key, sizeof(key), dst.key, base_name(argv[0]));
            return put_file(argv[0], dst.bucket, key);
        }
        return put_file(argv[0], dst.bucket, dst.key);
    }
    if (src_s3 && !dst_s3) {
        char local[1024];
        if (recursive) {
            return copy_remote_prefix_to_local(src.bucket, src.key, argv[1]);
        }
        if (path_is_dir(argv[1])) {
            join_path(local, sizeof(local), argv[1], base_name(src.key));
        } else {
            snprintf(local, sizeof(local), "%s", argv[1]);
        }
        return get_file(src.bucket, src.key, local);
    }
    if (recursive) {
        return copy_remote_prefix_to_remote(src.bucket, src.key, dst.bucket, dst.key);
    }
    return copy_remote(src.bucket, src.key, dst.bucket, dst.key[0] ? dst.key : src.key);
}

static int cmd_mv(int argc, char **argv) {
    s3_uri_t src, dst;
    int rc;
    int recursive = 0;
    int i;
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--recursive") == 0) {
            recursive = 1;
            break;
        }
    }
    if ((recursive && argc != 3) || (!recursive && argc != 2)) {
        fprintf(stderr, "Uso: aws-s3 mv origen destino\n");
        return 1;
    }
    parse_s3_uri(argv[0], &src);
    parse_s3_uri(argv[1], &dst);
    rc = cmd_cp(argc, argv);
    if (rc != 0) {
        return rc;
    }
    if (src.is_s3) {
        return delete_remote(src.bucket, src.key, recursive);
    }
    return unlink(argv[0]) == 0 ? 0 : 1;
}

static int cmd_rm(int argc, char **argv) {
    s3_uri_t u;
    int recursive = 0;
    int i;
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--recursive") == 0) {
            recursive = 1;
            argv[i] = argv[argc - 1];
            argc--;
            break;
        }
    }
    if (argc != 1 || parse_s3_uri(argv[0], &u) != 1 || !u.key[0]) {
        fprintf(stderr, "Uso: aws-s3 rm s3://bucket/key [--recursive]\n");
        return 1;
    }
    return delete_remote(u.bucket, u.key, recursive);
}

static int collect_local_and_upload(const char *root, const char *path,
                                    const char *bucket, const char *prefix,
                                    remote_list_t *remote, int *present) {
    DIR *dir;
    struct dirent *de;
    char current[1024];
    char key[S3_MAX_KEY];
    size_t root_len = strlen(root);

    if (path_is_file(path)) {
        const char *rel = path + root_len;
        int idx;
        if (*rel == '/' || *rel == '\\') rel++;
        snprintf(key, sizeof(key), "%s", rel);
        normalize_slashes(key);
        if (prefix && *prefix) {
            char tmp[S3_MAX_KEY];
            key_join(tmp, sizeof(tmp), prefix, key);
            strcpy(key, tmp);
        }
        idx = remote_index(remote, key);
        if (idx >= 0) {
            present[idx] = 1;
        }
        if (idx < 0 || remote->items[idx].size != file_size(path)) {
            printf("sync upload: %s\n", key);
            return put_file(path, bucket, key);
        }
        return 0;
    }

    dir = opendir(path);
    if (!dir) return 1;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        join_path(current, sizeof(current), path, de->d_name);
        if (collect_local_and_upload(root, current, bucket, prefix, remote, present) != 0) {
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
}

static int remote_has_local_key(remote_list_t *remote, const char *prefix,
                                const char *root, const char *path) {
    char key[S3_MAX_KEY];
    const char *rel = path + strlen(root);

    if (*rel == '/' || *rel == '\\') {
        rel++;
    }
    if (prefix && *prefix) {
        key_join(key, sizeof(key), prefix, rel);
    } else {
        snprintf(key, sizeof(key), "%s", rel);
    }
    normalize_slashes(key);
    return remote_index(remote, key) >= 0;
}

static int delete_extra_local(const char *root, const char *path,
                              const char *prefix, remote_list_t *remote) {
    DIR *dir;
    struct dirent *de;
    char current[1024];

    if (path_is_file(path)) {
        if (!remote_has_local_key(remote, prefix, root, path)) {
            printf("sync delete local: %s\n", path);
            return unlink(path) == 0 ? 0 : 1;
        }
        return 0;
    }

    dir = opendir(path);
    if (!dir) {
        return 0;
    }
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        join_path(current, sizeof(current), path, de->d_name);
        if (delete_extra_local(root, current, prefix, remote) != 0) {
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    if (strcmp(root, path) != 0) {
        rmdir(path);
    }
    return 0;
}

static int cmd_sync(int argc, char **argv) {
    s3_uri_t a, b;
    int a_s3;
    int b_s3;
    int del = 0;
    remote_list_t remote;
    int present[BUCKET_MAX_OBJECTS];
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--delete") == 0) {
            del = 1;
            for (; i + 1 < argc; i++) argv[i] = argv[i + 1];
            argc--;
            break;
        }
    }
    if (argc != 2) {
        fprintf(stderr, "Uso: aws-s3 sync origen destino [--delete]\n");
        return 1;
    }
    a_s3 = parse_s3_uri(argv[0], &a);
    b_s3 = parse_s3_uri(argv[1], &b);
    if (!a_s3 && b_s3) {
        memset(present, 0, sizeof(present));
        if (fetch_remote_list(b.bucket, b.key, &remote) != 0) return 1;
        if (collect_local_and_upload(argv[0], argv[0], b.bucket, b.key, &remote, present) != 0) return 1;
        if (del) {
            for (i = 0; i < remote.count; i++) {
                if (!present[i]) {
                    printf("sync delete remote: %s\n", remote.items[i].key);
                    delete_remote(b.bucket, remote.items[i].key, 0);
                }
            }
        }
        return 0;
    }
    if (a_s3 && !b_s3) {
        if (fetch_remote_list(a.bucket, a.key, &remote) != 0) return 1;
        for (i = 0; i < remote.count; i++) {
            char local[1024];
            const char *rel = remote.items[i].key + strlen(a.key);
            if (*rel == '/') rel++;
            join_path(local, sizeof(local), argv[1], rel);
            if (!path_is_file(local) || file_size(local) != remote.items[i].size) {
                printf("sync download: %s\n", remote.items[i].key);
                if (get_file(a.bucket, remote.items[i].key, local) != 0) return 1;
            }
        }
        if (del && delete_extra_local(argv[1], argv[1], a.key, &remote) != 0) {
            return 1;
        }
        (void)del;
        return 0;
    }
    fprintf(stderr, "sync soporta local->S3 o S3->local\n");
    return 1;
}

static int cmd_rb(int argc, char **argv) {
    s3_uri_t u;
    int force = 0;
    int i;
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) {
            force = 1;
            argv[i] = argv[argc - 1];
            argc--;
            break;
        }
    }
    if (argc != 1 || parse_s3_uri(argv[0], &u) != 1 || u.key[0]) {
        fprintf(stderr, "Uso: aws-s3 rb s3://bucket [--force]\n");
        return 1;
    }
    return send_remove_bucket(u.bucket, force);
}

static void usage(void) {
    fprintf(stderr,
            "Uso: aws-s3 [--host ip] [--port n] comando ...\n"
            "Comandos: ls, mb, cp, mv, rm, sync, rb\n");
}

int main(int argc, char **argv) {
    int i = 1;
    const char *cmd;

    while (i < argc && strncmp(argv[i], "--", 2) == 0) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[i + 1]);
            i += 2;
        } else {
            break;
        }
    }

    if (i >= argc) {
        usage();
        return 1;
    }
    cmd = argv[i++];

    if (strcmp(cmd, "ls") == 0) return cmd_ls(argc - i, argv + i);
    if (strcmp(cmd, "mb") == 0) {
        s3_uri_t u;
        if (argc - i != 1 || parse_s3_uri(argv[i], &u) != 1 || u.key[0]) {
            fprintf(stderr, "Uso: aws-s3 mb s3://bucket\n");
            return 1;
        }
        return send_create_bucket(u.bucket);
    }
    if (strcmp(cmd, "cp") == 0) return cmd_cp(argc - i, argv + i);
    if (strcmp(cmd, "mv") == 0) return cmd_mv(argc - i, argv + i);
    if (strcmp(cmd, "rm") == 0) return cmd_rm(argc - i, argv + i);
    if (strcmp(cmd, "sync") == 0) return cmd_sync(argc - i, argv + i);
    if (strcmp(cmd, "rb") == 0) return cmd_rb(argc - i, argv + i);

    usage();
    return 1;
}
