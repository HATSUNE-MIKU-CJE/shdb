#ifndef __SHDB_H
#define __SHDB_H

#include <stdint.h>

#define SHDB_MAGIC          0x53484442    //"SHDB"对应的16进制，用来表明自己是shdb数据文件
#define SHDB_VERSION        1             //版本号
#define SHDB_DATA_PATH      "data.shdb"   //数据文件名
#define RECORD_SIZE         36            //一条记录在磁盘上的字节数 
#define HEADER_SIZE         16            //头文件在磁盘上的字节数，record从16字节处开始
#define COMMAND_NUMBER      3             //命令语句数量
#define INSERT_ARGS_NUMBER  3             //insert命令包含参数数量
#define QUERY_ARGS_NUMBER   4
#define TIME_ARGS_NUMBER    7

typedef struct 
{
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint64_t created_at;                  //文件创建时的Unix毫秒时间戳
}__attribute__((packed)) file_header;

typedef struct 
{
    uint64_t timestamp;                   //Unix 毫秒时间戳
    uint8_t  class_id;                    //目标类别，填枚举值
    float    confidence;                  //置信度
    uint16_t bbox_x;                      //边界框左上角x
    uint16_t bbox_y;                      //边界框左上角y
    uint16_t bbox_w;                      //边界框宽度
    uint16_t bbox_h;                      //边界框高度
    uint32_t thumb_offset;                //缩略图偏移
    uint32_t thumb_size;                  //缩略图大小
    uint8_t  flags;                       //bit0=tombstone 删除标记
    uint8_t  reserved[2];                  //对齐填充
    uint32_t crc32;                       //CRC32校验
}__attribute__((packed)) detect_record ;

typedef struct 
{
    uint8_t  class_id;                    // --class 字符串转成的枚举值
    float    confidence;                  // --conf  
    uint16_t bbox[4];                     // --bbox  拆成x y w h
}insert_args;

typedef struct 
{
    uint64_t from_ts;
    uint64_t to_ts;
    uint8_t  class_id;
    float    min_conf;
    int      has_class;
    int      has_min_conf;
}query_args;


typedef enum
{
    CLASS_PERSON = 0,
    CLASS_OTHER  = 1,
    CLASS_MAX    = 2,
}class_id_t;


typedef int (*handle_fun_t)(int argc, char *argv[]);

typedef struct 
{
    char *name;
    handle_fun_t handler; 
}command;

int cmd_insert(int argc, char *argv[]);
int cmd_query(int argc, char *argv[]);
int cmd_info(int argc, char *argv[]);
int parse_insert_args(int argc, char *argv[], insert_args *insert_args);
int parse_query_args(int argc, char *argv[], query_args *query_args);
int dispatch(int argc, char *argv[]);
uint16_t swap16(uint16_t x);
uint32_t swap32(uint32_t x);
uint64_t swap64(uint64_t x);

extern char *class_id_table[CLASS_MAX];

#endif
