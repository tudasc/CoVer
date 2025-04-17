#pragma once

#include "stdint.h"

struct Tag_t {
    const char* tag;
    int64_t param;
};

struct TagsMap_t {
    void** functions; // List of function pointers
    Tag_t* tags; // List of tags (matching above funcptr)
    int64_t count;
};

struct CallOp_t {
    void** target_function;
};
struct CallTagOp_t {
    const char* target_tag;
};

// For operations: Number must match those defined in ContractTree.hpp!
enum ContractConnective : int64_t { UNARY_READ = 0, UNARY_WRITE = 1, UNARY_CALL = 2, UNARY_CALLTAG = 3, UNARY_RELEASE = 4, AND = 5, OR = 6, XOR = 7 };
struct ContractFormula_t {
    ContractFormula_t* children;
    int64_t num_children;
    ContractConnective conn;
    void** data; // Only filled if conn == UNARY. Pointer to corresponding operation struct.
};

struct Contract_t {
    ContractFormula_t precondition;
    ContractFormula_t postcondition;
    void* function;
    const char* function_name;
};

struct ContractDB_t {
    Contract_t* contracts;
    int64_t num_contracts;
    TagsMap_t tags;
};
