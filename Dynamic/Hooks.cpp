#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <cstdarg>

#include "DynamicAnalysis.h"
#include "DynamicUtils.h"

#include "Hooks.hpp"

extern "C" void __attribute__((visibility("default"))) PPDCV_Initialize(int32_t* argc, char*** argv, ContractDB_t const* DB) {
    DynamicUtils::createMessage("Initializing...");
    DynamicUtils::Initialize(DB);

    if (*argc >= 2) {
        std::string arg = (*argv)[1];
        if (arg == "--cover:check-coverage") {
            DynamicUtils::createMessage("Coverage check requested!");
            // Fill relevant locs
            std::unordered_set<Reference_t*> relevantLocs;
            for (int i = 0; i < DB->num_references; i++)
                relevantLocs.insert(&DB->references[i]);
            std::vector<std::pair<std::string, void*>> coverageVisited;
            for (std::filesystem::path const& entry : std::filesystem::directory_iterator(std::filesystem::current_path())) {
                if (entry.filename().string().starts_with("CoVerCoverage")) {
                    DynamicUtils::out() << "Reading coverage file " << entry.filename() << "...\n";
                    std::ifstream coverage_file(entry);
                    std::string line = "";
                    while (std::getline(coverage_file, line)) {
                        if (line.empty()) continue;
                        int pos = line.find_first_of('|');
                        std::string parsed_loc_s = line.substr(pos + 1);
                        long parsed_loc = std::strtoul(parsed_loc_s.c_str(), nullptr, 16);
                        coverageVisited.push_back({line.substr(0, pos), (void*)parsed_loc});
                    }
                }
            }
            if (!coverageVisited.empty()) DynamicUtils::out() << "Coverage read complete, no more coverage files detected. Checking...\n";
            else DynamicUtils::out() << "No coverage data found. Either program was not executed, or no relevant locations were encountered.\n";
            for (std::pair<std::string,void const*> loc : coverageVisited) {
                std::string locstr = DynamicUtils::getFileRefStr(loc.first, loc.second);
                std::erase_if(relevantLocs, [&](Reference_t* relRef){ return relRef->ref == locstr; });
            }
            if (!relevantLocs.empty()) {
                DynamicUtils::out() << "Coverage error detected!\n";
                for (Reference_t* const& unvisited : relevantLocs) {
                    DynamicUtils::out() << "Relevant location " << unvisited->ref << " of error \"" << unvisited->type << "\" not checked!\n";
                }
                exit(EXIT_FAILURE);
            } else {
                DynamicUtils::out() << "No coverage errors found.\n";
                exit(0);
            }
        }
    }

    visitedLocs.reserve(DB->num_references * 3); // Reserve more as some lines contain multiple callbacks

    // Create contract map and analyses for each operation
    for (int i = 0; i < DB->num_contracts; i++) {
        void* function = DB->contracts[i].function;
        if (!contrs.contains(function)) contrs[function] = {};
        contrs[function].push_back(DB->contracts[i]);
        if (DB->contracts[i].precondition) {
            recurseCreateAnalyses(DB->contracts[i].precondition, nullptr, true, function);
            toplevel_to_contract[DB->contracts[i].precondition] = &DB->contracts[i];
        }
        if (DB->contracts[i].postcondition) {
            recurseCreateAnalyses(DB->contracts[i].postcondition, nullptr, false, function);
            toplevel_to_contract[DB->contracts[i].postcondition] = &DB->contracts[i];
        }
    }

    DynamicUtils::out() << "Registered " << all_analyses.size() << " analyses\n";

    contract_status.reserve(formula_parents.size() + 2);

    atexit(PPDCV_destructor);

    DynamicUtils::createMessage("Finished Initializing!");
}

extern "C" void __attribute__((visibility("default"))) PPDCV_FunctionCallback(bool isRef, void* function, int32_t num_params, ...) {
    CallsiteInfo callsite = { .location = __builtin_return_address(0) };
    std::va_list list;
    va_start(list, num_params);
    for (int i = 0; i < num_params; i++) {
        uint32_t param_size = va_arg(list,uint32_t);
        void const* param_val = va_arg(list,void*);
        callsite.params.push_back({param_val, param_size});
    }
    va_end(list);

    // Run event handlers and remove analysis if done
    HANDLE_CALLBACK(analyses_with_funcCB, onFunctionCall, function, callsite);
}

extern "C" void __attribute__((visibility("default"))) PPDCV_MemRCallback(bool isRef, void const* buf) {
    HANDLE_CALLBACK(analyses_with_memRCB, onMemoryAccess, buf, false);
}
extern "C" void __attribute__((visibility("default"))) PPDCV_MemWCallback(bool isRef, void const* buf) {
    HANDLE_CALLBACK(analyses_with_memWCB, onMemoryAccess, buf, true);
}
