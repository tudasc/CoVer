#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
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

void PPDCV_Initialize(int32_t* argc, char*** argv, ContractDB_t const* DB) {
    DynamicUtils::createMessage("Initializing...");
    DynamicUtils::Initialize(DB);

    if (*argc >= 2) {
        std::string arg = (*argv)[1];
        if (arg == "--cover-check-coverage") {
            // Fill relevant locs
            std::unordered_set<std::string> relevantLocs;
            for (int i = 0; i < DB->num_references; i++)
                relevantLocs.insert(DB->references[i]);
            std::vector<std::pair<std::string, void*>> coverageVisited;
            DynamicUtils::createMessage("Coverage check requested!");
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
                relevantLocs.erase(locstr);
            }
            if (!relevantLocs.empty()) {
                DynamicUtils::out() << "Coverage error detected!\n";
                for (std::string const& unvisited : relevantLocs) {
                    DynamicUtils::out() << "Relevant location " << unvisited << " not checked!\n";
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

    atexit(PPDCV_destructor);

    DynamicUtils::createMessage("Finished Initializing!");
}

void PPDCV_FunctionCallback(bool isRef, void* function, int32_t num_params, ...) {
    CallsiteInfo callsite = { .location = __builtin_return_address(0) };
    std::va_list list;
    va_start(list, num_params);
    for (int i = 0; i < num_params; i++) {
        bool isPtr = va_arg(list, int32_t);
        int32_t param_size = va_arg(list, int32_t);
        if (isPtr) {
            callsite.params.push_back({va_arg(list,void*)});
        } else {
            if (param_size == 64)
                callsite.params.push_back({reinterpret_cast<void*>(va_arg(list, int64_t))});
            else if (param_size == 32)
                callsite.params.push_back({reinterpret_cast<void*>(va_arg(list, int32_t))});
            else
                DynamicUtils::createMessage("Unkown Parameter size!");
        }
    }
    va_end(list);

    // Run event handlers and remove analysis if done
    HANDLE_CALLBACK(analyses_with_funcCB, onFunctionCall, function, callsite);
}

void PPDCV_MemRCallback(bool isRef, void* buf) {
    HANDLE_CALLBACK(analyses_with_memRCB, onMemoryAccess, buf, false);
}
void PPDCV_MemWCallback(bool isRef, void* buf) {
    HANDLE_CALLBACK(analyses_with_memWCB, onMemoryAccess, buf, true);
}
