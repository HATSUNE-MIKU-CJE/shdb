#define _POSIX_C_SOURCE 199309L  //在使用clock_gettime时需要开启这个POSIX宏
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "shdb.h"

static int dp_create(char *path);

int cmd_insert(int argc, char *argv[]) 
{
    int fd = open(SHDB_DATA_PATH,O_RDWR);
    if (fd == -1)
    {
        fd = dp_create(SHDB_DATA_PATH);
        if (fd==-1)
        {
            return -1;
        }
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME,&ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    insert_args insert_args;
    parse_insert_args(argc, argv, &insert_args);
    detect_record shdb_record;
    memset(&shdb_record,0,sizeof(shdb_record));
    uint32_t raw;
    memcpy(&raw, &insert_args.confidence, 4);
    raw = swap32(raw);
    memcpy(&shdb_record.confidence, &raw, 4);
    shdb_record.timestamp = swap64(now_ms);
    shdb_record.class_id  = insert_args.class_id;
    shdb_record.bbox_x    = swap16(insert_args.bbox[0]);
    shdb_record.bbox_y    = swap16(insert_args.bbox[1]);
    shdb_record.bbox_w    = swap16(insert_args.bbox[2]);
    shdb_record.bbox_h    = swap16(insert_args.bbox[3]);
    lseek(fd,0,SEEK_END);
    if (write(fd,&shdb_record,sizeof(shdb_record))==-1)
    {
        perror("write failed");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int dp_create(char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd == -1)
    {
        perror("open failed");
        return -1;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME,&ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    file_header shdb_header;
    shdb_header.magic       = swap32 (SHDB_MAGIC);
    shdb_header.version     = swap16 (SHDB_VERSION);
    shdb_header.record_size = swap16 (RECORD_SIZE);
    shdb_header.created_at  = swap64 (now_ms);
    if (write(fd,&shdb_header,sizeof(shdb_header))==-1)
    {
        perror("write failed");
        close(fd);
        return -1;
    }
    return fd;
}
 