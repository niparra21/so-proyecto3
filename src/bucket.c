#include "bucket.h"

#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void bucket_path(char *out, size_t out_size, const char *bucket) {
    snprintf(out, out_size, "%s/%s%s", BUCKET_DIR, bucket, BUCKET_EXT);
}

static int valid_name(const char *s) {
    size_t i;
    if (!s || !*s || strlen(s) >= S3_MAX_BUCKET) {
        return 0;
    }
    for (i = 0; s[i]; i++) {
        if (!(isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_' || s[i] == '.')) {
            return 0;
        }
    }
    return 1;
}

static void init_header(bucket_header_t *h) {
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, BUCKET_MAGIC, 7);
}

static int read_header(FILE *f, bucket_header_t *h) {
    rewind(f);
    if (fread(h, 1, sizeof(*h), f) != sizeof(*h)) {
        return -1;
    }
    if (memcmp(h->magic, BUCKET_MAGIC, 7) != 0) {
        return -1;
    }
    return 0;
}

static int write_header(FILE *f, const bucket_header_t *h) {
    rewind(f);
    if (fwrite(h, 1, sizeof(*h), f) != sizeof(*h)) {
        return -1;
    }
    return fflush(f);
}

static FILE *open_bucket(const char *bucket, const char *mode) {
    char path[256];
    bucket_path(path, sizeof(path), bucket);
    return fopen(path, mode);
}

static int find_entry(bucket_header_t *h, const char *key) {
    int i;
    for (i = 0; i < BUCKET_MAX_OBJECTS; i++) {
        if (h->entries[i].used && strcmp(h->entries[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int free_entry_slot(bucket_header_t *h) {
    int i;
    for (i = 0; i < BUCKET_MAX_OBJECTS; i++) {
        if (!h->entries[i].used) {
            return i;
        }
    }
    return -1;
}

static int add_hole(bucket_header_t *h, uint64_t offset, uint64_t size) {
    int i;
    if (size == 0) {
        return 0;
    }
    for (i = 0; i < BUCKET_MAX_HOLES; i++) {
        if (!h->holes[i].used) {
            h->holes[i].used = 1;
            h->holes[i].offset = offset;
            h->holes[i].size = size;
            h->hole_count++;
            return 0;
        }
    }
    return -1;
}

static uint64_t allocate_space(FILE *f, bucket_header_t *h, uint64_t size) {
    int i;
    uint64_t offset;

    for (i = 0; i < BUCKET_MAX_HOLES; i++) {
        if (h->holes[i].used && h->holes[i].size >= size) {
            offset = h->holes[i].offset;
            if (h->holes[i].size == size) {
                h->holes[i].used = 0;
                h->hole_count--;
            } else {
                h->holes[i].offset += size;
                h->holes[i].size -= size;
            }
            return offset;
        }
    }

    fseek(f, 0, SEEK_END);
    offset = (uint64_t)ftell(f);
    if (offset < sizeof(bucket_header_t)) {
        offset = sizeof(bucket_header_t);
    }
    return offset;
}

int bucket_init_storage(void) {
    return mkdir_p(BUCKET_DIR);
}

int bucket_exists(const char *bucket) {
    char path[256];
    bucket_path(path, sizeof(path), bucket);
    return access(path, F_OK) == 0;
}

int bucket_create(const char *bucket) {
    char path[256];
    FILE *f;
    bucket_header_t h;

    if (!valid_name(bucket) || bucket_init_storage() < 0) {
        return -1;
    }
    if (bucket_exists(bucket)) {
        return -2;
    }
    bucket_path(path, sizeof(path), bucket);
    f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    init_header(&h);
    if (write_header(f, &h) < 0) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int bucket_is_empty(const char *bucket) {
    FILE *f = open_bucket(bucket, "rb");
    bucket_header_t h;
    if (!f) {
        return -1;
    }
    if (read_header(f, &h) < 0) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return h.entry_count == 0;
}

int bucket_remove(const char *bucket, int force) {
    char path[256];
    int empty;
    if (!bucket_exists(bucket)) {
        return -2;
    }
    empty = bucket_is_empty(bucket);
    if (empty < 0) {
        return -1;
    }
    if (!empty && !force) {
        return -3;
    }
    bucket_path(path, sizeof(path), bucket);
    return unlink(path);
}

int bucket_put_stream(const char *bucket, const char *key, int fd, uint64_t size) {
    FILE *f;
    bucket_header_t h;
    int idx;
    uint64_t offset;
    uint64_t left;
    char buf[8192];

    if (!bucket_exists(bucket) || !key || !*key || strlen(key) >= S3_MAX_KEY) {
        return -1;
    }
    f = open_bucket(bucket, "r+b");
    if (!f) {
        return -1;
    }
    if (read_header(f, &h) < 0) {
        fclose(f);
        return -1;
    }

    idx = find_entry(&h, key);
    if (idx >= 0 && h.entries[idx].size == size) {
        offset = h.entries[idx].offset;
    } else {
        if (idx < 0) {
            idx = free_entry_slot(&h);
            if (idx < 0) {
                fclose(f);
                return -1;
            }
            h.entry_count++;
        } else if (add_hole(&h, h.entries[idx].offset, h.entries[idx].size) < 0) {
            fclose(f);
            return -1;
        }
        offset = allocate_space(f, &h, size);
    }

    fseek(f, (long)offset, SEEK_SET);
    left = size;
    while (left > 0) {
        size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
        if (read_exact(fd, buf, want) < 0) {
            fclose(f);
            return -1;
        }
        if (fwrite(buf, 1, want, f) != want) {
            fclose(f);
            return -1;
        }
        left -= want;
    }

    h.entries[idx].used = 1;
    strncpy(h.entries[idx].key, key, sizeof(h.entries[idx].key) - 1);
    h.entries[idx].key[sizeof(h.entries[idx].key) - 1] = '\0';
    h.entries[idx].offset = offset;
    h.entries[idx].size = size;

    if (write_header(f, &h) < 0) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int bucket_put_file(const char *bucket, const char *key, const char *local_path) {
    FILE *in;
    FILE *f;
    bucket_header_t h;
    int idx;
    uint64_t offset;
    uint64_t size;
    char buf[8192];
    size_t n;

    if (!bucket_exists(bucket) || !path_is_file(local_path) || !key || !*key) {
        return -1;
    }
    in = fopen(local_path, "rb");
    f = open_bucket(bucket, "r+b");
    if (!in || !f) {
        if (in) fclose(in);
        if (f) fclose(f);
        return -1;
    }
    size = file_size(local_path);
    if (read_header(f, &h) < 0) {
        fclose(in);
        fclose(f);
        return -1;
    }
    idx = find_entry(&h, key);
    if (idx >= 0 && h.entries[idx].size == size) {
        offset = h.entries[idx].offset;
    } else {
        if (idx < 0) {
            idx = free_entry_slot(&h);
            if (idx < 0) {
                fclose(in);
                fclose(f);
                return -1;
            }
            h.entry_count++;
        } else if (add_hole(&h, h.entries[idx].offset, h.entries[idx].size) < 0) {
            fclose(in);
            fclose(f);
            return -1;
        }
        offset = allocate_space(f, &h, size);
    }

    fseek(f, (long)offset, SEEK_SET);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, f) != n) {
            fclose(in);
            fclose(f);
            return -1;
        }
    }
    if (ferror(in)) {
        fclose(in);
        fclose(f);
        return -1;
    }

    h.entries[idx].used = 1;
    strncpy(h.entries[idx].key, key, sizeof(h.entries[idx].key) - 1);
    h.entries[idx].key[sizeof(h.entries[idx].key) - 1] = '\0';
    h.entries[idx].offset = offset;
    h.entries[idx].size = size;
    if (write_header(f, &h) < 0) {
        fclose(in);
        fclose(f);
        return -1;
    }
    fclose(in);
    fclose(f);
    return 0;
}

int bucket_send_object(const char *bucket, const char *key, int fd) {
    FILE *f;
    bucket_header_t h;
    int idx;
    uint64_t left;
    char buf[8192];

    f = open_bucket(bucket, "rb");
    if (!f || read_header(f, &h) < 0) {
        if (f) fclose(f);
        return -1;
    }
    idx = find_entry(&h, key);
    if (idx < 0) {
        fclose(f);
        return -2;
    }
    send_linef(fd, "OK %llu\n", (unsigned long long)h.entries[idx].size);
    fseek(f, (long)h.entries[idx].offset, SEEK_SET);
    left = h.entries[idx].size;
    while (left > 0) {
        size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
        if (fread(buf, 1, want, f) != want || write_exact(fd, buf, want) < 0) {
            fclose(f);
            return -1;
        }
        left -= want;
    }
    fclose(f);
    return 0;
}

int bucket_get_to_file(const char *bucket, const char *key, const char *local_path) {
    FILE *src;
    FILE *dst;
    bucket_header_t h;
    int idx;
    uint64_t left;
    char buf[8192];

    src = open_bucket(bucket, "rb");
    if (!src || read_header(src, &h) < 0) {
        if (src) fclose(src);
        return -1;
    }
    idx = find_entry(&h, key);
    if (idx < 0) {
        fclose(src);
        return -2;
    }
    if (parent_mkdir_p(local_path) < 0) {
        fclose(src);
        return -1;
    }
    dst = fopen(local_path, "wb");
    if (!dst) {
        fclose(src);
        return -1;
    }
    fseek(src, (long)h.entries[idx].offset, SEEK_SET);
    left = h.entries[idx].size;
    while (left > 0) {
        size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
        if (fread(buf, 1, want, src) != want || fwrite(buf, 1, want, dst) != want) {
            fclose(src);
            fclose(dst);
            return -1;
        }
        left -= want;
    }
    fclose(src);
    fclose(dst);
    return 0;
}

int bucket_copy_object(const char *src_bucket, const char *src_key,
                       const char *dst_bucket, const char *dst_key) {
    char tmp[] = "/tmp/aws-s3-copy-XXXXXX";
    int fd = mkstemp(tmp);
    int rc;

    if (fd < 0) {
        return -1;
    }
    close(fd);
    rc = bucket_get_to_file(src_bucket, src_key, tmp);
    if (rc == 0) {
        rc = bucket_put_file(dst_bucket, dst_key, tmp);
    }
    unlink(tmp);
    return rc;
}

int bucket_delete_key(const char *bucket, const char *key) {
    FILE *f;
    bucket_header_t h;
    int idx;

    f = open_bucket(bucket, "r+b");
    if (!f || read_header(f, &h) < 0) {
        if (f) fclose(f);
        return -1;
    }
    idx = find_entry(&h, key);
    if (idx < 0) {
        fclose(f);
        return -2;
    }
    if (add_hole(&h, h.entries[idx].offset, h.entries[idx].size) < 0) {
        fclose(f);
        return -1;
    }
    memset(&h.entries[idx], 0, sizeof(h.entries[idx]));
    h.entry_count--;
    if (write_header(f, &h) < 0) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int bucket_delete_prefix(const char *bucket, const char *prefix) {
    FILE *f;
    bucket_header_t h;
    int i;
    int removed = 0;
    size_t plen = strlen(prefix);

    f = open_bucket(bucket, "r+b");
    if (!f || read_header(f, &h) < 0) {
        if (f) fclose(f);
        return -1;
    }
    for (i = 0; i < BUCKET_MAX_OBJECTS; i++) {
        if (h.entries[i].used && strncmp(h.entries[i].key, prefix, plen) == 0) {
            add_hole(&h, h.entries[i].offset, h.entries[i].size);
            memset(&h.entries[i], 0, sizeof(h.entries[i]));
            h.entry_count--;
            removed++;
        }
    }
    if (write_header(f, &h) < 0) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return removed;
}

int bucket_list_keys(const char *bucket, const char *prefix,
                     bucket_list_cb cb, void *ctx) {
    FILE *f;
    bucket_header_t h;
    int i;
    size_t plen = prefix ? strlen(prefix) : 0;

    f = open_bucket(bucket, "rb");
    if (!f || read_header(f, &h) < 0) {
        if (f) fclose(f);
        return -1;
    }
    for (i = 0; i < BUCKET_MAX_OBJECTS; i++) {
        if (h.entries[i].used &&
            (!prefix || strncmp(h.entries[i].key, prefix, plen) == 0)) {
            cb(h.entries[i].key, h.entries[i].size, ctx);
        }
    }
    fclose(f);
    return 0;
}

int bucket_list_objects(FILE *out) {
    DIR *dir;
    struct dirent *de;
    size_t ext_len = strlen(BUCKET_EXT);

    if (bucket_init_storage() < 0) {
        return -1;
    }
    dir = opendir(BUCKET_DIR);
    if (!dir) {
        return -1;
    }
    while ((de = readdir(dir)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len > ext_len && strcmp(de->d_name + len - ext_len, BUCKET_EXT) == 0) {
            fprintf(out, "%.*s\n", (int)(len - ext_len), de->d_name);
        }
    }
    closedir(dir);
    return 0;
}

int bucket_stat_key(const char *bucket, const char *key, uint64_t *size) {
    FILE *f;
    bucket_header_t h;
    int idx;

    f = open_bucket(bucket, "rb");
    if (!f || read_header(f, &h) < 0) {
        if (f) fclose(f);
        return -1;
    }
    idx = find_entry(&h, key);
    if (idx < 0) {
        fclose(f);
        return -2;
    }
    *size = h.entries[idx].size;
    fclose(f);
    return 0;
}
