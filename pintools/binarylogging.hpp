#ifndef BINARYLOGGING_HPP
#define BINARYLOGGING_HPP

#include <stdint.h>

typedef struct AccessLogEntry_t {
    uint64_t time;
    char type;
    void * addr;
    uint32_t size;
    uint32_t codeAddr;
    uint64_t value;
    void * rtnAddr;
    void * allocBase;
} AccessLogEntry;


typedef struct FunctionLogEntry_t {
    uint64_t time;
    char name[100];
    char type;
    uint32_t tid;
} FunctionLogEntry;

typedef struct AllocLogEntry_t {
    uint64_t time;
    void *allocPoint;
    char type;
    void * addr;
    uint32_t size;
    uint32_t codeAddr;
    void * rtnAddr;
    void * allocId;
} AllocLogEntry;

#endif
