#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define S3_DEFAULT_HOST "127.0.0.1"
#define S3_DEFAULT_PORT 9090
#define S3_MAX_LINE 1024
#define S3_MAX_KEY 256
#define S3_MAX_BUCKET 64

typedef struct {
    char bucket[S3_MAX_BUCKET];
    char key[S3_MAX_KEY];
    int is_s3;
} s3_uri_t;

int parse_s3_uri(const char *text, s3_uri_t *out);
int connect_to_server(const char *host, int port);
int create_server_socket(int port);
int read_exact(int fd, void *buf, size_t size);
int write_exact(int fd, const void *buf, size_t size);
int read_line(int fd, char *buf, size_t size);
int send_linef(int fd, const char *fmt, ...);
int copy_stream(FILE *in, int out_fd, uint64_t size);
int recv_to_file(int in_fd, FILE *out, uint64_t size);
int mkdir_p(const char *path);
int parent_mkdir_p(const char *path);
int path_is_dir(const char *path);
int path_is_file(const char *path);
uint64_t file_size(const char *path);
void normalize_slashes(char *s);
const char *base_name(const char *path);
void join_path(char *out, size_t out_size, const char *left, const char *right);

#endif
