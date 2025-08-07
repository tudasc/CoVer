#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdarg>

#include "DynamicAnalysis.h"
#include "Analyses/BaseAnalysis.h"
#include "Analyses/PreCallAnalysis.h"
#include "Analyses/PostCallAnalysis.h"
#include "Analyses/ReleaseAnalysis.h"
#include "DynamicUtils.h"

ContractDB_t* DB = nullptr;

struct ErrorMessage {
    std::vector<std::string> msg;
    std::vector<ErrorMessage> child_msg;
};

std::unordered_map<void*, std::vector<Contract_t>> contrs;

std::unordered_map<ContractFormula_t*,Fulfillment> contract_status;
std::unordered_map<std::shared_ptr<BaseAnalysis>,ContractFormula_t*> analyses;
std::unordered_set<void*> called_funcs;

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

ErrorMessage recurseResolveFormula(ContractFormula_t* form) {
    if (form->num_children == 0) {
        if (contract_status[form] == Fulfillment::FULFILLED) return {};
        switch (form->conn) {
            case UNARY_CALL:
            case UNARY_CALLTAG: {
                CallTagOp_t* cOP = (CallTagOp_t*)form->data;
                return {{std::string("Operation Message (if defined): ") + form->msg,
                              std::string("Did not find call to ") + cOP->target_tag}, {}};
            }
            case UNARY_RELEASE: {
                ReleaseOp_t* rOP = (ReleaseOp_t*)form->data;
                return {{std::string("Operation Message (if defined): ") + form->msg,
                              std::string("Found forbidden operation!")}, {}};
            }
            default: return {{"UNEXPECTED OPERATION IN RESOLVE STEP"}, {}};
        }
    }
    std::vector<ErrorMessage> child_msg;
    for (int i = 0; i < form->num_children; i++) {
        child_msg.push_back(recurseResolveFormula(&form->children[i]));
    }
    switch (form->conn) {
        case AND: {
            bool all_success = std::all_of(child_msg.begin(), child_msg.end(), [](ErrorMessage const& err) { return err.msg.empty(); });
            if (all_success) return {{}, child_msg};
            else return {{std::string("A child is not satisfied for Formula: ") + form->msg}, child_msg};
        }
        case OR: {
            bool any_success = std::any_of(child_msg.begin(), child_msg.end(), [](ErrorMessage const& err) { return err.msg.empty(); });
            if (any_success) return {{}, child_msg};
            else return {{std::string("No child satisfied for Formula: ") + form->msg}, child_msg};
        }
        case XOR: {
            bool found = false;
            for (ErrorMessage const& cmsg : child_msg) {
                if (cmsg.msg.empty()) {
                    if (found) return {{std::string("More than one child satisfied for Formula: ") + form->msg}, child_msg};
                    found = true;
                }
            }
            if (!found) return {{std::string("No child satisfied for Formula: ") + form->msg}, child_msg};
            return {{}, child_msg};
        }
        default: return {{"UNEXPECTED CONNECTIVE IN RESOLVE STEP"}, {}};
    }
}

void formatError(ErrorMessage msg, int indent = 2) {
    if (msg.msg.empty()) return;
    std::string indent_s(indent, ' ');
    for (std::string const& line : msg.msg)
        DynamicUtils::out() << indent_s << "- " << line << "\n";
    for (ErrorMessage child : msg.child_msg) {
        formatError(child, indent + 2);
    }
}

void resolveContracts() {
    for (std::pair<void*, std::vector<Contract_t>> contract : contrs) {
        if (!called_funcs.contains(contract.first)) continue;
        for (Contract_t C : contract.second) {
            ErrorMessage errors_precond = C.precondition ? recurseResolveFormula(C.precondition) : ErrorMessage{};
            ErrorMessage errors_postcond = C.postcondition ? recurseResolveFormula(C.postcondition) : ErrorMessage{};
            if (errors_precond.msg.empty() && errors_postcond.msg.empty()) continue;
            DynamicUtils::out() << "Error in contract for function \"" << C.function_name << "\":\n";
            if (!errors_precond.msg.empty()) {
                DynamicUtils::out() << "  Precondition:\n";
                formatError(errors_precond);
            }
            if (!errors_postcond.msg.empty()) {
                DynamicUtils::out() << "  Precondition:\n";
                formatError(errors_postcond);
            }
        }
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
        if (DB->contracts[i].precondition) {
            recurseCreateAnalyses(DB->contracts[i].precondition, true, function);
        }
        if (DB->contracts[i].postcondition) {
            recurseCreateAnalyses(DB->contracts[i].postcondition, false, function);
        }
    }

    DynamicUtils::createMessage("Finished Initializing!");
    return;
}

void PPDCV_FunctionCallback(void* function, int64_t num_params, ...) {
    called_funcs.insert(function);

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

    // Run event handlers and remove analysis if done
    std::erase_if(
        analyses,
        [&](std::pair<std::shared_ptr<BaseAnalysis>,ContractFormula_t*> analysis) {
            Fulfillment f = analysis.first->onFunctionCall(__builtin_return_address(0), function, callsite_params);
            if (f != Fulfillment::UNKNOWN) contract_status[analysis.second] = f;
            return f != Fulfillment::UNKNOWN;
        }
    );
}

void PPDCV_MemCallback(int64_t isWrite, void* buf) {
    std::erase_if(
        analyses,
        [&](std::pair<std::shared_ptr<BaseAnalysis>,ContractFormula_t*> analysis) {
            Fulfillment f = analysis.first->onMemoryAccess(__builtin_return_address(0), buf, isWrite);
            if (f != Fulfillment::UNKNOWN) contract_status[analysis.second] = f;
            return f != Fulfillment::UNKNOWN;
        }
    );
}

extern "C" __attribute__((destructor)) void PPDCV_destructor() {
    #warning todo find better way for postprocessing
    std::cout << "CoVer-Dynamic: Analysis finished.\n";
    for (std::pair<std::shared_ptr<BaseAnalysis>,ContractFormula_t*> analysis : analyses) {
        Fulfillment f = analysis.first->onProgramExit(__builtin_return_address(0));
        contract_status[analysis.second] = f;
    }
    resolveContracts();
}
