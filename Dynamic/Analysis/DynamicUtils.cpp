#include "DynamicUtils.h"

#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <ios>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <array>

#include "DynamicAnalysis.h"

namespace {
    std::string exec(std::string const& cmd) {
        std::array<char, 128> buffer;
        std::string result;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            DynamicUtils::createMessage("popen() failed! File references will be broken");
        }
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            result += buffer.data();
        }
        int ret = WEXITSTATUS(pclose(pipe));
        if (ret != 0) {
            exit(ret);
        }
        return result;
    }
}

namespace DynamicUtils {
    std::unordered_map<void*, std::unordered_set<Tag_t*>> func_to_tags;
    std::unordered_map<std::string, std::unordered_set<void*>> tags_to_func;

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

    bool checkParamMatch(ParamAccess acc, void const* contrP, void const* callP) {
        switch (acc) {
            case ParamAccess::NORMAL:
                return contrP == callP;
            case ParamAccess::DEREF:
                return *(void**)contrP == callP;
            case ParamAccess::ADDROF:
                return *(void**)callP == contrP;
        }
    }

    std::unordered_set<void*> getFunctionsForTag(std::string tag) {
        if (tags_to_func.contains(tag))
            return tags_to_func[tag];
        return {};
    }

    std::unordered_set<Tag_t*> getTagsForFunction(void* func) {
        if (func_to_tags.contains(func))
            return func_to_tags[func];
        return {};
    }

    void createMessage(std::string msg) {
        out() << msg << "\n";
        std::flush(std::cerr);
    }
    std::ostream& out() {
        return std::cerr << "CoVer-Dynamic: ";
    }

    bool checkFuncCallMatch(void* callF, std::vector<CallParam_t*> params_expect, CallsiteInfo callParams, CallsiteInfo contrParams, std::string target_str) {
        for (CallParam_t* param : params_expect) {
            if (param->callPisTagVar) {
                std::unordered_set<Tag_t*> tags = DynamicUtils::getTagsForFunction(callF);
                for (Tag_t* tag : tags) {
                    if (tag->tag != target_str) continue;
                    if (DynamicUtils::checkParamMatch(param->accType, contrParams.params[param->contrP], callParams.params[tag->param]))
                        return true;
                }
            } else {
                if (DynamicUtils::checkParamMatch(param->accType, contrParams.params[param->contrP], callParams.params[param->callP]))
                    return true;
            }
        }
        return false;
    }

    std::string getFileReference(void const* location) {
        std::string filename = std::getenv("COVER_DYNAMIC_FILENAME");
        std::stringstream exec_cmd;
        Dl_info info;
        if (!dladdr(location, &info)) {
            return "dladdr failure!";
        }
        intptr_t resolvedLocation = (intptr_t)location;
        if ((intptr_t)info.dli_fbase != (intptr_t)0x400000) {
            // if the filebase is not 0x400000, we have an VMA offset that we have to subtract
            // otherwise, it is a PIE so we can just use the codePtr
            // Additionally, subtract 1 to get from return address to call
            location = (void*)((intptr_t)location - (intptr_t)info.dli_fbase - 1);
        }
#ifdef CMAKE_ADDR2LINE
        exec_cmd << CMAKE_ADDR2LINE " -e " << filename << " " << std::hex << location;
        std::string result = exec(exec_cmd.str());
        result.pop_back();
#else
        exec_cmd << filename << std::hex << "[" << location << "] (Cannot resolve: No addr2line support configured)\n";
        std::string result = exec_cmd.str();
#endif
        return result;
    }
}
