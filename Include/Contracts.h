#pragma once

#include <stdbool.h>
#include <stdint.h>

// Convenience Annotation Macros
// Example: int f() CONTRACT( <Contract String> );
#define CONTRACT(...) __attribute__((annotate("CONTRACT{" #__VA_ARGS__ "}")))
#define CONTRACTXF(...) __attribute__((annotate("CONTRACTXFAIL{" #__VA_ARGS__ "}")))
#define CONTRACTXS(...) __attribute__((annotate("CONTRACTXSUCC{" #__VA_ARGS__ "}")))

// Contract Value Names Definitions

// DO NOT USE THE STRUCT DEFINITIONS DIRECTLY!
// Use CONTRACT_VALUE_PAIR macro instead
struct ContractValuePair {
    const char* name;
    void* value;
} __attribute__((packed)) typedef ContractValuePair_t;

#define CONCAT_IMPL( x, y ) x##y
#define MACRO_CONCAT( x, y ) CONCAT_IMPL( x, y )

// Define a name for a constant value
// Example: CONTRACT_VALUE_PAIR(zero,0)
#define CONTRACT_VALUE_PAIR(x,y) \
    ContractValuePair_t MACRO_CONCAT(ContractValueInfo_, __COUNTER__ ) __attribute__((used)) = {#x, (void*)y};
