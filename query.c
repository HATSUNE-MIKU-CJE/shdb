#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "shdb.h"


int cmd_query(int argc, char *argv[])
{
    detect_id id;
    detect_record shdb_record;
    query_args query_args;
    time_t sec;
    char buf[64];
    int i = 0;
    struct tm time_info;
    int fd = open(SHDB_DATA_PATH,O_RDONLY);
    if (fd == -1)
    {
        perror("open failed");
        return -1;
    }
    parse_query_args(argc,argv,&query_args);
    while (1)
    {
        lseek(fd,HEADER_SIZE+i*RECORD_SIZE+i*ID_SIZE,SEEK_SET);
        ssize_t n =read(fd,&shdb_record,RECORD_SIZE);
        ssize_t m =read(fd,&id,ID_SIZE);
        i++;
        if (n==RECORD_SIZE)
        {
            if (shdb_record.flags == TOMBSTONE_FLAG)
            {
                continue;
            }
            uint32_t stored = swap32(shdb_record.crc32);
            uint32_t computed = crc32((uint8_t*)&shdb_record,32);
            if (stored != computed)
            {
                fprintf(stderr,"warning: CRC mismatch, skipping record\n");
                continue;
            }
            shdb_record.timestamp = swap64(shdb_record.timestamp);
            shdb_record.bbox_x = swap16(shdb_record.bbox_x);
            shdb_record.bbox_y = swap16(shdb_record.bbox_y);
            shdb_record.bbox_w = swap16(shdb_record.bbox_w);
            shdb_record.bbox_h = swap16(shdb_record.bbox_h);
            uint32_t raw;
            memcpy(&raw, &shdb_record.confidence, 4);
            raw = swap32(raw);
            memcpy(&shdb_record.confidence, &raw, 4);
            if (shdb_record.timestamp<query_args.from_ts || shdb_record.timestamp>query_args.to_ts)
            {continue;}
            if (query_args.has_class==1)
            {
                if (shdb_record.class_id != query_args.class_id)
                {continue;}
            }
            if (query_args.has_min_conf==1)
            {
                if (shdb_record.confidence < query_args.min_conf)
                {continue;}
            }
            sec = shdb_record.timestamp / 1000;
            localtime_r(&sec,&time_info);
            strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",&time_info);
            printf("time: %s class: %s confidence: %f x: %d y: %d w: %d h: %d",
                    buf, class_id_table[shdb_record.class_id], 
                    shdb_record.confidence, shdb_record.bbox_x,
                    shdb_record.bbox_y,shdb_record.bbox_w,shdb_record.bbox_h);
            
        }
        else if (n==0)
        {
            break;
        }
        
        else
        {
            perror("read failed");
            close(fd);
            return -1;
        }
        if (m == ID_SIZE)
        {
            id.id = swap32(id.id); 
            printf(" #id: %d\n",id.id);
        }
        else
        {
            perror("read failed");
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}

int cmd_info(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    uint32_t free_slot[MAX_RECORDS];
    file_header file_header;
    struct stat st;
    long int size;
    int records;
    time_t sec;
    struct tm time_info;
    char buf[64];
    if (stat(SHDB_DATA_PATH,&st)==-1)
    {
        perror("stat failed");
        return -1;
    }
    size = st.st_size;
    int fd = open(SHDB_DATA_PATH,O_RDONLY);
    if (fd == -1)
    {
        perror("open failed");
        return -1;
    }
    uint32_t free_count = find_free_id(free_slot,fd);
    lseek(fd,0,SEEK_SET);
    if (read(fd,&file_header,HEADER_SIZE)==HEADER_SIZE)
    {
        file_header.magic = swap32(file_header.magic);
        file_header.version = swap16(file_header.version);
        file_header.record_size = swap16(file_header.record_size);
        file_header.created_at = swap64(file_header.created_at);
        records = (size - HEADER_SIZE) / (RECORD_SIZE+ID_SIZE);
        sec = file_header.created_at / 1000;
        localtime_r(&sec,&time_info);
        strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",&time_info);
        printf("created_at: %s\nmagic: %u\nversion: %d\ntotal_records: %d\nalive_records: %d\nrecord_size: %d\n",
            buf, file_header.magic, file_header.version, records, records-free_count, file_header.record_size);
    }
    else
    {
        perror("read failed");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}
