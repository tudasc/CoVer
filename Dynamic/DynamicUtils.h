#pragma once

#include "DynamicAnalysis.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <unordered_set>
#include <string>
#include <sstream>
#include <vector>

#ifdef NDEBUG
#define ANALYSIS_PREAMBLE inline __attribute__((always_inline))
#else
#define ANALYSIS_PREAMBLE
#endif

using CodePtr = void const*;
struct ConcreteParam {
    void const* value;
    uint32_t size;
    bool operator==(ConcreteParam const& other) const { return value == other.value; }
};
struct CallsiteInfo {
    CodePtr location;
    std::vector<ConcreteParam> params;
    bool operator==(CallsiteInfo const& other) const {
        return this->location == other.location && params == other.params;
    }
};

template <>
struct std::hash<CallsiteInfo> {
    std::size_t operator()(CallsiteInfo const& ref) const
    {
        std::size_t hash = std::hash<decltype(ref.location)>()(ref.location);
        for (ConcreteParam p : ref.params)
            hash ^= std::hash<decltype(p.value)>()(p.value);
        return hash;
    }
};

namespace DynamicUtils {
    // Initialize Utils
    void Initialize(ContractDB_t const* DB);

    // Check if two parameters match
    bool checkParamMatch(ParamAccess const& acc, ConcreteParam const& contrP, ConcreteParam const& callP);

    // Check if function call matches
    bool checkFuncCallMatch(void const* callF, std::vector<CallParam_t*> params_expect, CallsiteInfo callParams, CallsiteInfo contrParams, std::string target_str);

    // Resolve tag to possible functions
    std::vector<void const*> getFunctionsForTag(std::string tag);

    // Resolve function to possible tags
    std::vector<Tag_t*> getTagsForFunction(void const* func);

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
