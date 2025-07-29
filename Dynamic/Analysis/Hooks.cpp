#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdarg>

#include "DynamicAnalysis.h"
#include "Analyses/BaseAnalysis.h"
#include "Analyses/PreCallAnalysis.h"
#include "Analyses/PostCallAnalysis.h"
#include "Analyses/ReleaseAnalysis.h"
#include "DynamicUtils.h"

ContractDB_t* DB = nullptr;



std::map<void*, std::vector<Contract_t>> contrs;
std::map<void*, std::string> interesting_funcs;

std::map<std::shared_ptr<BaseAnalysis>,ContractFormula_t*> analyses;

void recurseCreateAnalyses(ContractFormula_t* form, bool isPre, void* func_supplier) {
    if (form->num_children == 0) {
        switch (form->conn) {
            case UNARY_CALL:
                if (isPre) analyses.insert({std::make_shared<PreCallAnalysis>(func_supplier, (CallOp_t*)form->data), form});
                else analyses.insert({std::make_shared<PostCallAnalysis>(func_supplier, (CallOp_t*)form->data), form});
                break;
            case UNARY_CALLTAG:
                if (isPre) analyses.insert({std::make_shared<PreCallAnalysis>(func_supplier, (CallTagOp_t*)form->data), form});
                else analyses.insert({std::make_shared<PostCallAnalysis>(func_supplier, (CallTagOp_t*)form->data), form});
                break;
            case UNARY_RELEASE:
                if (isPre) DynamicUtils::createMessage("Did not expect releaseop in precond!");
                else analyses.insert({std::make_shared<ReleaseAnalysis>(func_supplier, (ReleaseOp_t*)form->data), form});
                break;
            default: 
                DynamicUtils::createMessage("Unknown top-level operation!");
                break;
        }
    } else {
        for (int i = 0; i < form->num_children; i++) recurseCreateAnalyses(&form->children[i], isPre, func_supplier);
    }
}

void PPDCV_Initialize(ContractDB_t* _DB) {
    DynamicUtils::createMessage("Initializing...");
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

    DynamicUtils::createMessage("Finished Initializing!");
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
        [&](std::pair<std::shared_ptr<BaseAnalysis>,ContractFormula_t*> analysis) {
            Fulfillment f = analysis.first->onFunctionCall(__builtin_return_address(0), function, callsite_params);
            if (f == Fulfillment::VIOLATED) DynamicUtils::createMessage("Error for: " + std::string(analysis.second->msg));
            return f != Fulfillment::UNKNOWN;
        }
    );
}

void PPDCV_MemCallback(int64_t isWrite, void* buf) {
    std::erase_if(
        analyses,
        [&](std::pair<std::shared_ptr<BaseAnalysis>,ContractFormula_t*> analysis) {
            Fulfillment f = analysis.first->onMemoryAccess(__builtin_return_address(0), buf, isWrite);
            if (f == Fulfillment::VIOLATED) DynamicUtils::createMessage("Error for: " + std::string(analysis.second->msg));
            return f != Fulfillment::UNKNOWN;
        }
    );
}

extern "C" __attribute__((destructor)) void PPDCV_destructor() {
    #warning todo find better way for postprocessing
    std::cout << "CoVer-Dynamic: Analysis finished.\n";
    for (std::pair<std::shared_ptr<BaseAnalysis>,ContractFormula_t*> analysis : analyses) {
        Fulfillment f = analysis.first->onProgramExit(__builtin_return_address(0));
        if (f == Fulfillment::VIOLATED)
            DynamicUtils::createMessage("Error on program exit: " + std::string(analysis.second->msg));
    }
}
