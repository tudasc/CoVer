#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <vector>
#include <cstdarg>

#include "DynamicAnalysis.h"
#include "Analyses/BaseAnalysis.h"
#include "Analyses/PreCallAnalysis.h"
#include "DynamicUtils.h"

ContractDB_t* DB = nullptr;



std::map<void*, std::vector<Contract_t>> contrs;
std::map<void*, std::string> interesting_funcs;

std::vector<std::shared_ptr<BaseAnalysis>> analyses;

void recurseCreateAnalyses(ContractFormula_t* form, bool isPre, void* func_supplier) {
    if (form->num_children == 0) {
        switch (form->conn) {
            case UNARY_CALL:
                if (isPre) analyses.push_back(std::make_shared<PreCallAnalysis>(func_supplier, (CallOp_t*)form->data));
                else {
                    #warning todo
                    return;
                }
                break;
            case UNARY_CALLTAG:
                if (isPre) analyses.push_back(std::make_shared<PreCallAnalysis>(func_supplier, (CallTagOp_t*)form->data));
                else {
                    #warning todo
                    return;
                }
                break;
            default: 
                #warning TODO
                break;
        }
    } else {
        for (int i = 0; i < form->num_children; i++) recurseCreateAnalyses(&form->children[i], isPre, func_supplier);
    }
}

void PPDCV_Initialize(ContractDB_t* _DB) {
    std::cerr << "[MUST-CV] Initializing...\n";
    DB = _DB;

    DynamicUtils::Initialize(DB);

    // Create contract map, functions of interest, and analyses for each operation
    for (int i = 0; i < DB->num_contracts; i++) {
        void* function = DB->contracts[i].function;
        if (!contrs.contains(function)) contrs[function] = {};
        contrs[function].push_back(DB->contracts[i]);
        std::map<void*,std::string> new_funcs_pre;
        std::map<void*,std::string> new_funcs_post;
        if (DB->contracts[i].precondition) {
            new_funcs_pre = DynamicUtils::recurseGetFunctions(*DB->contracts[i].precondition);
            recurseCreateAnalyses(DB->contracts[i].precondition, true, function);
        }
        if (DB->contracts[i].postcondition) {
            new_funcs_post = DynamicUtils::recurseGetFunctions(*DB->contracts[i].postcondition);
            recurseCreateAnalyses(DB->contracts[i].postcondition, false, function);
        }
        interesting_funcs.insert(new_funcs_pre.begin(), new_funcs_pre.end());
        interesting_funcs.insert(new_funcs_post.begin(), new_funcs_post.end());
        interesting_funcs[function] = DB->contracts[i].function_name;
    }

    std::cerr << "[MUST-CV] Finished initializing!\n";
    return;
}

void PPDCV_FunctionCallback(void* function, int64_t num_params, ...) {
    CallsiteParams callsite_params;
    std::va_list list;
    va_start(list, num_params);
    for (int i = 0; i < num_params; i++) {
        bool isPtr = va_arg(list, int64_t);
        int64_t param_size = va_arg(list, int64_t);
        if (isPtr) {
            callsite_params.push_back({va_arg(list,void*), isPtr});
        } else {
            if (param_size == 64)
                callsite_params.push_back({(void*)va_arg(list, int64_t), isPtr});
            else if (param_size == 32)
                callsite_params.push_back({(void*)va_arg(list, int32_t), isPtr});
            else
                throw "Unkown parameter size!";
        }
    }
    va_end(list);
    if (!interesting_funcs.contains(function)) {
        std::cerr << "[MUST-CV] Instrumentation exists for unknown function!\n";
        return;
    }

    // Run event handlers and remove analysis if done
    std::erase_if(
        analyses,
        [&](std::shared_ptr<BaseAnalysis> analysis) {
            return analysis->onFunctionCall(__builtin_return_address(0), function, callsite_params) != Fulfillment::UNKNOWN;
        }
    );
}

void PPDCV_MemCallback(int64_t isWrite, void* buf) {
    // if (forbidden_release.contains(buf)) {
    //     int i = 0;
    //     std::set<ReleaseOp_t*> to_remove;
    //     for (ReleaseOp_t* relOp : forbidden_release[buf]) {
    //         if (release_processed.contains(relOp)) {
    //             to_remove.insert(relOp); // Already released. Just cleanup needed
    //         } else {
    //             if (relOp->forbidden_op_kind == UNARY_READ && !isWrite || relOp->forbidden_op_kind == UNARY_WRITE && isWrite) {
    //                 std::cerr << "Error!\n"; // Requirement violated
    //                 release_processed[relOp] = VIOLATED;
    //                 to_remove.insert(relOp);
    //             }
    //         }
    //     }
    //     for (ReleaseOp_t* relOp : to_remove) {
    //         forbidden_release[buf].erase(relOp);
    //     }
    // }
}
