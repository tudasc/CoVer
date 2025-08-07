#pragma once

#include "DynamicAnalysis.h"
#include <ostream>
#include <unordered_set>
#include <string>
#include <vector>

struct ConcreteParam {
    void* val;
    bool isPtr;
};
using CallsiteParams = std::vector<ConcreteParam>;

namespace DynamicUtils {
    // Initialize Utils
    void Initialize(ContractDB_t* DB);

    // Check if two parameters match
    bool checkParamMatch(ParamAccess acc, void* contrP, void* callP);

    // Check if function call matches
    bool checkFuncCallMatch(void* callF, std::vector<CallParam_t*> params_expect, CallsiteParams callParams, CallsiteParams contrParams, std::string target_str);

    // Resolve tag to possible functions
    std::unordered_set<void*> getFunctionsForTag(std::string tag);

    // Resolve function to possible tags
    std::unordered_set<Tag_t*> getTagsForFunction(void* func);

    // Report something
    void createMessage(std::string msg);

    // Report something (ostream)
    std::ostream& out();
}
