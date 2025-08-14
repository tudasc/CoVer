#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
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

struct AnalysisPair {
    ContractFormula_t* formula;
    std::variant<PreCallAnalysis,PostCallAnalysis,ReleaseAnalysis> analysis;
};

std::unordered_map<ContractFormula_t*, Fulfillment> contract_status;
std::vector<AnalysisPair> all_analyses;
std::vector<AnalysisPair> analyses_with_funcCB;
std::vector<AnalysisPair> analyses_with_memRCB;
std::vector<AnalysisPair> analyses_with_memWCB;
std::unordered_map<ContractFormula_t*, std::unordered_set<void*>> analysis_references;
std::unordered_set<void*> called_funcs;

template<typename Analysis, typename... Arguments>
inline void addAnalysis(ContractFormula_t* form, Arguments... args) {
    Analysis A = Analysis(args...);
    AnalysisPair new_pair = {form, A};
    all_analyses.push_back(new_pair);
    
    CallBacks reqCB = A.requiredCallbacks();
    if (reqCB.FUNCTION) analyses_with_funcCB.push_back(new_pair);
    if (reqCB.MEMORY_R) analyses_with_memRCB.push_back(new_pair);
    if (reqCB.MEMORY_W) analyses_with_memWCB.push_back(new_pair);
}

#define HANDLE_CALLBACK(pairs, CB, ...) \
    for (AnalysisPair& pair : pairs) { \
        std::visit([&](auto& analysis) { \
            Fulfillment f = analysis.CB(std::move(__builtin_return_address(0)), __VA_ARGS__); \
            if (f != Fulfillment::UNKNOWN) { \
                contract_status[pair.formula] = f; \
                analysis_references[pair.formula] = analysis.getReferences(); \
            } \
        }, pair.analysis); \
    }

void recurseCreateAnalyses(ContractFormula_t* form, bool isPre, void* func_supplier) {
    if (form->num_children == 0) {
        switch (form->conn) {
            case UNARY_CALL:
                if (isPre) addAnalysis<PreCallAnalysis>(form, func_supplier, (CallOp_t*)form->data);
                else addAnalysis<PostCallAnalysis>(form, func_supplier, (CallOp_t*)form->data);
                break;
            case UNARY_CALLTAG:
                if (isPre) addAnalysis<PreCallAnalysis>(form, func_supplier, (CallTagOp_t*)form->data);
                else addAnalysis<PostCallAnalysis>(form, func_supplier, (CallTagOp_t*)form->data);
                break;
            case UNARY_RELEASE:
                if (isPre) DynamicUtils::createMessage("Did not expect releaseop in precond!");
                else addAnalysis<ReleaseAnalysis>(form, func_supplier, (ReleaseOp_t*)form->data);
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
        ErrorMessage msg;
        switch (form->conn) {
            case UNARY_CALL:
            case UNARY_CALLTAG: {
                CallTagOp_t* cOP = (CallTagOp_t*)form->data;
                msg.msg = {std::string("Operation Message (if defined) or contract string: ") + form->msg,
                           std::string("Did not find call to ") + cOP->target_tag};
                break;
            }
            case UNARY_RELEASE: {
                ReleaseOp_t* rOP = (ReleaseOp_t*)form->data;
                msg.msg = {std::string("Operation Message (if defined) or contract string: ") + form->msg,
                           std::string("Found forbidden operation!")};
                break;
            }
            default: return {{"UNEXPECTED OPERATION IN RESOLVE STEP"}, {}};
        }
        std::unordered_set<void*> references = analysis_references[form];
        for (void* loc : references)
            msg.msg.push_back(std::string("Reference: ") + DynamicUtils::getFileReference(loc));
        return msg;
    }
    std::vector<ErrorMessage> child_msg;
    for (int i = 0; i < form->num_children; i++) {
        child_msg.push_back(recurseResolveFormula(&form->children[i]));
    }
    switch (form->conn) {
        case AND: {
            bool all_success = std::all_of(child_msg.begin(), child_msg.end(), [](ErrorMessage const& err) { return err.msg.empty(); });
            if (all_success) return {{}, child_msg};
            else return {{std::string("A child is not satisfied for Formula (message or contract string): ") + form->msg}, child_msg};
        }
        case OR: {
            bool any_success = std::any_of(child_msg.begin(), child_msg.end(), [](ErrorMessage const& err) { return err.msg.empty(); });
            if (any_success) return {{}, child_msg};
            else return {{std::string("No child satisfied for Formula (message or contract string): ") + form->msg}, child_msg};
        }
        case XOR: {
            bool found = false;
            for (ErrorMessage const& cmsg : child_msg) {
                if (cmsg.msg.empty()) {
                    if (found) return {{std::string("More than one child satisfied for Formula (message or contract string): ") + form->msg}, child_msg};
                    found = true;
                }
            }
            if (!found) return {{std::string("No child satisfied for Formula (message or contract string): ") + form->msg}, child_msg};;
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
    void* location = __builtin_return_address(0);
    called_funcs.insert(function);

    CallsiteInfo callsite_params;
    std::va_list list;
    va_start(list, num_params);
    for (int i = 0; i < num_params; i++) {
        bool isPtr = va_arg(list, int64_t);
        int64_t param_size = va_arg(list, int64_t);
        if (isPtr) {
            callsite_params.params.push_back({va_arg(list,void*)});
        } else {
            if (param_size == 64)
                callsite_params.params.push_back({(void*)va_arg(list, int64_t)});
            else if (param_size == 32)
                callsite_params.params.push_back({(void*)va_arg(list, int32_t)});
            else
                DynamicUtils::createMessage("Unkown Parameter size!");
        }
    }
    va_end(list);

    // Run event handlers and remove analysis if done
    HANDLE_CALLBACK(analyses_with_funcCB, onFunctionCall, function, callsite_params);
}

void PPDCV_MemCallback(int64_t isWrite, void* buf) {
    if (!isWrite) {
        HANDLE_CALLBACK(analyses_with_memRCB, onMemoryAccess, buf, false);
    } else {
        HANDLE_CALLBACK(analyses_with_memWCB, onMemoryAccess, buf, true);
    }
}

extern "C" __attribute__((destructor)) void PPDCV_destructor() {
    #warning todo find better way for postprocessing
    std::cout << "CoVer-Dynamic: Analysis finished.\n";
    for (AnalysisPair& pair : all_analyses) {
        std::visit([&](auto&& analysis) {
            if (contract_status.contains(pair.formula)) return;
            contract_status[pair.formula] = analysis.onProgramExit(std::move(__builtin_return_address(0)));
            analysis_references[pair.formula] = analysis.getReferences();
        }, pair.analysis);
    }
    resolveContracts();
}
