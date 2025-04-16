#pragma once

#include "stdint.h"

struct Tag_t {
    const char* tag;
    int64_t param;
};

struct TagsMap_t {
    void* functions; // List of function pointers
    Tag_t* tags; // List of tags (matching above funcptr)
    int64_t count;
};

struct ContractDB_t {
    TagsMap_t tags;
    TagsMap_t tags1;
};
