#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "shdb.h"

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
            insert_args->confidence = strtof (argv[i+1],NULL);
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

static command command_table[COMMAND_NUMBER] = {
    {"insert" , cmd_insert},
    {"query"  , cmd_query},
    {"info"   , cmd_info},
};

static char *insert_args_table[INSERT_ARGS_NUMBER] = {
    "--class",
    "--conf",
    "--bbox"
};

static char *class_id_table[CLASS_MAX] = {
    [CLASS_PERSON] = "person",
    [CLASS_OTHER]  = "other"
};