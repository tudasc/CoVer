#pragma once

#include "DynamicAnalysis.h"
#include <functional>
#include <optional>
#include <ostream>
#include <unordered_set>
#include <string>
#include <sstream>
#include <vector>

using ConcreteParam = void*;
struct CallsiteInfo {
    void* location;
    std::vector<ConcreteParam> params;
    bool operator==(CallsiteInfo const& other) const {
        return this->location == other.location && params == other.params;
    }
};

template <>
struct std::hash<CallsiteInfo> {
    std::size_t operator()(CallsiteInfo const& ref) const
    {
        std::size_t hash = std::hash<void*>()(ref.location);
        for (ConcreteParam p : ref.params)
            hash ^= std::hash<ConcreteParam>()(p);
        return hash;
    }
};

namespace DynamicUtils {
    // Initialize Utils
    void Initialize(ContractDB_t const* DB);

    // Check if two parameters match
    bool checkParamMatch(ParamAccess acc, void const* contrP, void const* callP);

    // Check if function call matches
    bool checkFuncCallMatch(void* callF, std::vector<CallParam_t*> params_expect, CallsiteInfo callParams, CallsiteInfo contrParams, std::string target_str);

    // Resolve tag to possible functions
    std::unordered_set<void*> getFunctionsForTag(std::string tag);

    // Resolve function to possible tags
    std::unordered_set<Tag_t*> getTagsForFunction(void* func);

    // Report something
    void createMessage(std::string msg);

    // Report something (ostream)
    std::ostream& out();

    // Resolve loc ptr to printable string
    std::string getFileRefStr(void const* location);
    std::string getFileRefStr(std::string file, void const* parsed_loc);

    // Get information needed for references
    std::optional<std::pair<std::string, void const*>> getDLInfo(void const* location);
}
