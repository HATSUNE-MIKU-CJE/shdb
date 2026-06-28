#define _POSIX_C_SOURCE 199309L  //在使用clock_gettime时需要开启这个POSIX宏
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "shdb.h"

static int dp_create(char *path);

int find_free_id(uint32_t free_slots[],int fd)
{
    int i = 0;
    int j = 0;
    detect_id id;
    detect_record shdb_record;
    while (1)
    {
        lseek(fd,HEADER_SIZE+i*RECORD_SIZE+i*ID_SIZE,SEEK_SET);
        i++;
        ssize_t n = read(fd,&shdb_record,RECORD_SIZE);
        if (n==RECORD_SIZE)
        {
            if (shdb_record.flags == TOMBSTONE_FLAG)
            {
                lseek(fd,0,SEEK_CUR);
                ssize_t m = read(fd,&id,ID_SIZE);
                if (m==4)
                {
                    free_slots[j] = swap32(id.id);
                    j++;
                }
                else
                {
                    perror("read failed");
                    close(fd);
                    return -1;
                }
            }
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
    }
    return j;
}

int cmd_insert(int argc, char *argv[]) 
{
    int fd = open(SHDB_DATA_PATH,O_RDWR);

    if (fd == -1)
    {
        if (errno == ENOENT)
        {
            fd = dp_create(SHDB_DATA_PATH);
            if (fd==-1)
            {
                return -1;
            }
        }
        else if (errno == EACCES)
        {perror("permission denied"); return -1;}
        else {perror("open failed"); return -1;}
    }
    uint32_t free_slots[MAX_RECORDS];
    uint32_t free_count = find_free_id(free_slots,fd);

    char tmp_path[256];
    snprintf(tmp_path,sizeof(tmp_path),"%s.tmp",SHDB_DATA_PATH);
    int fd_tmp = open(tmp_path,O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_tmp == -1)
    {
        if (errno == ENOSPC)
        {perror("disk full, cannot create temp file");}
        else{
            perror("open failed");
        }
        return -1;
    }
    file_header file_header;
    detect_record shdb_record;
    detect_id id;
    detect_id last_id = {-1};
    int i = 0;
    lseek(fd,0,SEEK_SET);
    if (read(fd,&file_header,HEADER_SIZE)==HEADER_SIZE)
    {
        if (write(fd_tmp,&file_header,HEADER_SIZE)==-1)
        {
            if (errno == ENOSPC) {perror("disk full during write");}
            else {perror("write failed");}
            close (fd);
            close (fd_tmp);
            return -1;
        }
    }
    else
    {
        perror("read header failed");
        close(fd);
        close(fd_tmp);
        return -1;
    }
    while (1)
    {
        lseek(fd,HEADER_SIZE+i*RECORD_SIZE+i*ID_SIZE,SEEK_SET);
        ssize_t n = read(fd,&shdb_record,RECORD_SIZE);
        i++;
        lseek(fd,0,SEEK_CUR);
        ssize_t m = read(fd,&id,ID_SIZE);
        if (n == RECORD_SIZE)
        {
            lseek(fd_tmp,HEADER_SIZE+(i-1)*RECORD_SIZE+(i-1)*ID_SIZE,SEEK_SET);
            if (write(fd_tmp,&shdb_record,RECORD_SIZE)==-1)
            {
                if (errno == ENOSPC) {perror("disk full during write");}
                else {perror("write failed");}
                close(fd);
                close(fd_tmp);
                return -1;
            }
        }
        else if (n==0)
        {
            break;
        }
        else
        {
            perror("read record failed");
            close(fd);
            close(fd_tmp);
            return -1;
        }
        if (m==ID_SIZE)
        {
            last_id.id = swap32 (id.id);
            id.id = swap32(id.id);
            lseek(fd_tmp,0,SEEK_CUR);
            if (write(fd_tmp,&id,ID_SIZE)==-1)
            {
                if (errno == ENOSPC) {perror("disk full during write");}
                else {perror("write failed");}
                close(fd);
                close(fd_tmp);
                return -1;
            }
        }
        else
        {
            perror("read id failed");
            close(fd);
            close(fd_tmp);
            return -1;
        }
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME,&ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    insert_args insert_args;
    parse_insert_args(argc, argv, &insert_args);
    memset(&shdb_record,0,sizeof(shdb_record));
    uint32_t raw;
    memcpy(&raw, &insert_args.confidence, 4);
    raw = swap32(raw);
    memcpy(&shdb_record.confidence, &raw, 4);
    shdb_record.timestamp = swap64(now_ms);
    shdb_record.flags     = 0;
    shdb_record.class_id  = insert_args.class_id;
    shdb_record.bbox_x    = swap16(insert_args.bbox[0]);
    shdb_record.bbox_y    = swap16(insert_args.bbox[1]);
    shdb_record.bbox_w    = swap16(insert_args.bbox[2]);
    shdb_record.bbox_h    = swap16(insert_args.bbox[3]);
    shdb_record.crc32     = swap32(crc32((uint8_t*)&shdb_record,32));
    if (free_count > 0)
    {
        last_id.id = free_slots[0];
        lseek(fd_tmp,HEADER_SIZE+free_slots[0]*RECORD_SIZE+free_slots[0]*ID_SIZE,SEEK_SET);
        if (write(fd_tmp,&shdb_record,RECORD_SIZE)==-1)
        {
            if (errno == ENOSPC) {perror("disk full during write");}
            else {perror("write failed");}
            close(fd);
            close(fd_tmp);
            return -1;
        }
        last_id.id = swap32(last_id.id);
        if (write(fd_tmp,&last_id,ID_SIZE)==-1)
        {
            if (errno == ENOSPC) {perror("disk full during write");}
            else {perror("write failed");}
            close(fd);
            close(fd_tmp);
            return -1;
        }
    }
    else
    {
        last_id.id+=1;
        lseek(fd_tmp,0,SEEK_END);
        if (write(fd_tmp,&shdb_record,RECORD_SIZE)==-1)
        {
            if (errno == ENOSPC) {perror("disk full during write");}
            else {perror("write failed");}
            close(fd_tmp);
            close(fd);
            return -1;
        }
        last_id.id = swap32(last_id.id);
        if (write(fd_tmp,&last_id,ID_SIZE)==-1)
        {
            if (errno == ENOSPC) {perror("disk full during write");}
            else {perror("write failed");}
            close(fd_tmp);
            close(fd);
            return -1;
        }
    }
    if (fsync(fd_tmp)==-1)
    {
        if (errno == EIO)
        {perror("disk I/O error during sync, data may be lost");}
        else
        {perror("fsync failed");}
        close (fd_tmp);
        close (fd);
        return -1;
    }
    if (rename(tmp_path,SHDB_DATA_PATH)==-1)
    {
        if (errno == EXDEV)
        {perror("tmp file and data file must be on same filesystem");}
        else
        {perror("rename failed");}
        close (fd_tmp);
        close (fd);
        return -1;
    }
    close(fd);
    close(fd_tmp);
    return 0;   

}

static int dp_create(char *path)
{
    char tmp_path[256];
    snprintf(tmp_path,sizeof(tmp_path),"%s.tmp",path);
    int fd1 = open(tmp_path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd1 == -1)
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
    if (write(fd1,&shdb_header,HEADER_SIZE)==-1)
    {
        if (errno == ENOSPC) {perror("disk full during write");}
        else {perror("write failed");}
        close(fd1);
        return -1;
    }
    if (fsync(fd1) == -1)
    {
        if (errno == EIO) {perror("disk I/O error during sync, data may be lost");}
        else {perror("fsync failed");}
        close(fd1);
        return -1;
    }
    if (rename(tmp_path,path)==-1)
    {
        if (errno == EXDEV) {perror("tmp file and data file must be on same filesystem");}
        else {perror("rename failed");}
        close(fd1);
        return -1;
    }
    close (fd1);
    int fd = open(path,O_RDWR);
    if (fd == -1)
    {
        perror("open failed");
        return -1;
    }
    return fd;
}

int cmd_delete(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    int fd = open(SHDB_DATA_PATH,O_RDWR);
    if (fd == -1)
    {
        perror("open failed");
        return -1;
    }
    int i = 0;
    detect_id id;
    delete_args delete_args;
    detect_record shdb_record;
    parse_delete_args(argc,argv,&delete_args);
    while (1)
    {
        lseek(fd,HEADER_SIZE+i*RECORD_SIZE+i*ID_SIZE,SEEK_SET);
        ssize_t n = read (fd,&shdb_record,RECORD_SIZE);
        ssize_t m = read (fd,&id,ID_SIZE);
        i++;
        if (m == ID_SIZE)
        {
            id.id = swap32(id.id);
            if (id.id == delete_args.id)
            {
                if (n == RECORD_SIZE)
                {
                    shdb_record.flags = TOMBSTONE_FLAG;
                    lseek(fd,HEADER_SIZE+(i-1)*RECORD_SIZE+(i-1)*ID_SIZE,SEEK_SET);
                    if (write(fd,&shdb_record,RECORD_SIZE)==-1)
                    {
                        if (errno == ENOSPC) {perror("disk full during write");}
                        else {perror("write failed");}
                        close(fd);
                        return -1;
                    }
                    if (write(fd,&id,ID_SIZE)==-1)
                    {
                        if (errno == ENOSPC) {perror("disk full during write");}
                        else {perror("write failed");}
                        close(fd);
                        return -1;
                    }
                    break;
                }
                else
                {
                    perror("write failed");
                    close(fd);
                    return -1;
                }
            }
        }
        else if (m == 0)
        {
            break;
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