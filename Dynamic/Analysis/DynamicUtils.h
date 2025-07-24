#pragma once

#include "DynamicAnalysis.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace DynamicUtils {
    // Initialize Utils
    void Initialize(ContractDB_t* DB);
    
    // Get all functions referenced in a contract
    std::map<void*,std::string> recurseGetFunctions(ContractFormula_t contr);

    // Check if two parameters match
    bool checkParamMatch(ParamAccess acc, void* contrP, void* callP);

    // Resolve tag to possible functions
    std::set<void*> getFunctionsForTag(std::string tag);

    // Resolve function to possible tags
    std::set<Tag_t> getTagsForFunction(void* func);

    // Report something
    void createMessage(std::string msg);
}
