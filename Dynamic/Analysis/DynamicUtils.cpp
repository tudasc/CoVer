#include "DynamicUtils.h"

#include <iostream>
#include <map>
#include <ostream>
#include <set>
#include <string>

#include "DynamicAnalysis.h"

namespace DynamicUtils {
    std::map<void*, std::set<Tag_t*>> func_to_tags;
    std::map<std::string, std::set<void*>> tags_to_func;

    void Initialize(ContractDB_t* DB) {
        // Initialize Tags
        for (int i = 0; i < DB->tagMap.count; i++) {
            void* function = DB->tagMap.functions[i];
            Tag_t* tag = &DB->tagMap.tags[i];
            if (!func_to_tags.contains(function)) func_to_tags[function] = {tag};
            else func_to_tags[function].insert(tag);
            if (!tags_to_func.contains(tag->tag)) tags_to_func[tag->tag] = {function};
            else tags_to_func[tag->tag].insert(function);
        }
    }

    bool checkParamMatch(ParamAccess acc, void* contrP, void* callP) {
        switch (acc) {
            case ParamAccess::NORMAL:
                return contrP == callP;
            case ParamAccess::DEREF:
                return *(void**)contrP == callP;
            case ParamAccess::ADDROF:
                return *(void**)callP == contrP;
        }
    }

    std::set<void*> getFunctionsForTag(std::string tag) {
        if (tags_to_func.contains(tag))
            return tags_to_func[tag];
        return {};
    }

    std::set<Tag_t*> getTagsForFunction(void* func) {
        if (func_to_tags.contains(func))
            return func_to_tags[func];
        return {};
    }

    void createMessage(std::string msg) {
        std::cerr << "CoVer-Dynamic: " << msg << "\n";
        std::flush(std::cerr);
    }

    bool checkFuncCallMatch(void* callF, std::vector<CallParam_t*> params_expect, CallsiteParams callParams, CallsiteParams contrParams, std::string target_str) {
        for (CallParam_t* param : params_expect) {
            if (param->callPisTagVar) {
                std::set<Tag_t*> tags = DynamicUtils::getTagsForFunction(callF);
                for (Tag_t* tag : tags) {
                    if (tag->tag != target_str) continue;
                    if (DynamicUtils::checkParamMatch(param->accType, contrParams[param->contrP].val, callParams[tag->param].val))
                        return true;
                }
            } else {
                if (DynamicUtils::checkParamMatch(param->accType, contrParams[param->contrP].val, callParams[param->callP].val))
                    return true;
            }
        }
        return false;
    }
}
