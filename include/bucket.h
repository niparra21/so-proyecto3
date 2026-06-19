#ifndef BUCKET_H
#define BUCKET_H

#include <stdint.h>
#include <stdio.h>

#include "common.h"

#define BUCKET_DIR "buckets"
#define BUCKET_EXT ".bucket"
#define BUCKET_MAGIC "S3BKT01"
#define BUCKET_MAX_OBJECTS 512
#define BUCKET_MAX_HOLES 512

typedef struct {
    int used;
    char key[S3_MAX_KEY];
    uint64_t offset;
    uint64_t size;
} bucket_entry_t;

typedef struct {
    int used;
    uint64_t offset;
    uint64_t size;
} bucket_hole_t;

typedef struct {
    char magic[8];
    uint32_t entry_count;
    uint32_t hole_count;
    bucket_entry_t entries[BUCKET_MAX_OBJECTS];
    bucket_hole_t holes[BUCKET_MAX_HOLES];
} bucket_header_t;

typedef void (*bucket_list_cb)(const char *key, uint64_t size, void *ctx);

int bucket_init_storage(void);
int bucket_create(const char *bucket);
int bucket_remove(const char *bucket, int force);
int bucket_exists(const char *bucket);
int bucket_is_empty(const char *bucket);
int bucket_put_file(const char *bucket, const char *key, const char *local_path);
int bucket_put_stream(const char *bucket, const char *key, int fd, uint64_t size);
int bucket_get_to_file(const char *bucket, const char *key, const char *local_path);
int bucket_send_object(const char *bucket, const char *key, int fd);
int bucket_copy_object(const char *src_bucket, const char *src_key,
                       const char *dst_bucket, const char *dst_key);
int bucket_delete_key(const char *bucket, const char *key);
int bucket_delete_prefix(const char *bucket, const char *prefix);
int bucket_list_keys(const char *bucket, const char *prefix,
                     bucket_list_cb cb, void *ctx);
int bucket_list_objects(FILE *out);
int bucket_stat_key(const char *bucket, const char *key, uint64_t *size);

#endif
