#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include "shdb.h"

static command command_table[COMMAND_NUMBER] = {
    {"insert" , cmd_insert},
    {"query"  , cmd_query},
    {"info"   , cmd_info},
    {"delete" , cmd_delete},
};

static char *insert_args_table[INSERT_ARGS_NUMBER] = {
    "--class",
    "--conf",
    "--bbox"
};

static char *query_args_table[QUERY_ARGS_NUMBER] = {
    "--from",
    "--to",
    "--class",
    "--min-conf"
};

static char *delete_args_table[DELETE_ARGS_NUMBER] = {
    "--id"
};

char *class_id_table[CLASS_MAX] = {
    [CLASS_PERSON] = "person",
    [CLASS_OTHER]  = "other"
};

int dispatch(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr,"usage: shdb <insert|query|info> ... \n");
        return 1;
    }
    for (int i=0;i<COMMAND_NUMBER;i++)
    {
        if (strcmp(command_table[i].name,argv[1])==0)
        {
            return command_table[i].handler(argc-2,argv+2);
        }
    }
    fprintf(stderr, "unknown command: %s\n",argv[1]);
    return 1; 

}

int parse_insert_args(int argc, char *argv[], insert_args *insert_args)
{
    for (int i=0;i<argc;i++)
    {
        if (strcmp(insert_args_table[0],argv[i])==0)
        {
            for (int j=0;j<CLASS_MAX;j++)
            {
                if (strcmp(class_id_table[j],argv[i+1])==0)
                {
                    insert_args->class_id = j;
                }
            }
        }
        else if (strcmp(insert_args_table[1],argv[i])==0)
        {
            errno = 0;
            insert_args->confidence = strtof (argv[i+1],NULL);
            if (errno == ERANGE)
            {
                fprintf(stderr,"number out of range: %s\n",argv[i+1]);
                return -1;
            }
        }
        else if (strcmp(insert_args_table[2],argv[i])==0)
        {
            char *token;
            token = strtok(argv[i+1],",");
            for (int k=0;k<4;k++)
            {
                insert_args->bbox[k] = (uint16_t) strtoul(token,NULL,10);
                token = strtok(NULL,",");
            }
        }
    }
    return 0;
}

static int parse_time(char *time, struct tm *time_info)
{
    int i=0;
    char *token_0;
    char *token;
    char *token_1;
    char *save0;
    char *save;
    char *save1;
    int *time_table[TIME_ARGS_NUMBER] = {
        &time_info->tm_year,
        &time_info->tm_mon,
        &time_info->tm_mday,
        &time_info->tm_hour,
        &time_info->tm_min,
        &time_info->tm_sec,
        &time_info->tm_isdst,
    };
    token_0 = strtok_r(time," ",&save0);
    token = strtok_r(token_0,"-",&save);
    while (token != NULL && i<3)
    {
        switch (i)
        {
        case 0:
            *time_table[i] = (int)strtoul(token,NULL,10) - 1900;
            break;
        case 1:
            *time_table[i] = (int)strtoul(token,NULL,10) - 1;
            break;
        case 2:
            *time_table[i] = (int)strtoul(token,NULL,10);
            break;
        default:
            break;
        }
        token = strtok_r(NULL,"-",&save);
        i++;
    }
    token_0 = strtok_r(NULL," ",&save0);
    token_1 = strtok_r(token_0,":",&save1);
    while (token_1 != NULL)
    {
        switch (i)
        {
        case 3:
            *time_table[i] = (int)strtoul(token_1,NULL,10);
            break;
        case 4:
            *time_table[i] = (int)strtoul(token_1,NULL,10);
            break;
        case 5:
            *time_table[i] = (int)strtoul(token_1,NULL,10);
            break;
        case 6:
            *time_table[i] = -1;
            break;
        default:
            break;
        }
        token_1 = strtok_r(NULL,":",&save1);
        i++;
    }
    return 0;

}

int parse_query_args(int argc, char *argv[], query_args *query_args)
{
    struct tm from;
    struct tm to;
    query_args->has_class = 0;
    query_args->has_min_conf = 0;
    for (int i=0;i<argc;i++)
    {
        if (strcmp(query_args_table[0],argv[i])==0)
        {
            parse_time(argv[i+1],&from);
            query_args->from_ts = (uint64_t)mktime(&from) * 1000;
        }
        else if (strcmp(query_args_table[1],argv[i])==0)
        {
            parse_time(argv[i+1],&to);
            query_args->to_ts = (uint64_t)mktime(&to) *1000;
        }
        else if (strcmp(query_args_table[2],argv[i])==0)
        {
            query_args->has_class = 1;
            for (int j=0;j<CLASS_MAX;j++)
            {
                if (strcmp(class_id_table[j],argv[i+1])==0)
                {
                    query_args->class_id = j;
                }
            }
        }
        else if (strcmp(query_args_table[3],argv[i])==0)
        {
            query_args->has_min_conf = 1;
            query_args->min_conf = strtof(argv[i+1],NULL);
        }
    }
    return 0;
}

int parse_delete_args(int argc, char *argv[], delete_args *delete_args)
{
    (void)argc;
    if (strcmp(delete_args_table[0],argv[0])==0)
    {
        delete_args->id = (uint32_t)strtoul(argv[1],NULL,10);
    }
    return 0;
}
