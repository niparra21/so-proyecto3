CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS ?=

COMMON_OBJS = build/common.o build/bucket.o

.PHONY: all clean test

all: aws-s3 aws-s3_server

build:
	mkdir -p build

aws-s3: build src/client.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) src/client.c $(COMMON_OBJS) -o $@ $(LDFLAGS)

aws-s3_server: build src/server.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) src/server.c $(COMMON_OBJS) -o $@ $(LDFLAGS)

build/common.o: build src/common.c include/common.h
	$(CC) $(CFLAGS) -c src/common.c -o $@

build/bucket.o: build src/bucket.c include/bucket.h include/common.h
	$(CC) $(CFLAGS) -c src/bucket.c -o $@

clean:
	rm -rf build aws-s3 aws-s3_server buckets

test: all
	sh tests/smoke.sh
