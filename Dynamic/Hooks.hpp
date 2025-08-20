#include <algorithm>
#include <format>
#include <iostream>
#include <fstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <variant>
#include <ctime>
#include <vector>

#include "FastVariant.h"

#include "Analyses/PreCallAnalysis.h"
#include "Analyses/PostCallAnalysis.h"
#include "Analyses/ReleaseAnalysis.h"
#include "DynamicAnalysis.h"

namespace {
    struct ErrorMessage {
        std::vector<std::string> msg;
        std::vector<ErrorMessage> child_msg;
    };

    std::unordered_map<void*, std::vector<Contract_t>> contrs;

    struct AnalysisPair {
        ContractFormula_t* formula;
        std::variant<PreCallAnalysis*,PostCallAnalysis*,ReleaseAnalysis*> analysis;
    };

    void PPDCV_destructor();

    std::unordered_set<void const*> visitedLocs;

    std::unordered_map<ContractFormula_t*, Fulfillment> contract_status;
    std::vector<AnalysisPair> all_analyses;
    std::vector<AnalysisPair> analyses_with_funcCB;
    std::vector<AnalysisPair> analyses_with_memRCB;
    std::vector<AnalysisPair> analyses_with_memWCB;
    std::unordered_map<ContractFormula_t*, std::vector<void const*>> analysis_references;
    std::unordered_set<void*> called_funcs;

    template<typename Analysis, typename... Arguments>
    inline void addAnalysis(ContractFormula_t* form, Arguments... args) {
        Analysis* A = new Analysis(args...);
        AnalysisPair new_pair = {form, A};
        all_analyses.push_back(new_pair);
        
        CallBacks reqCB = A->requiredCallbacks();
        if (reqCB.FUNCTION) analyses_with_funcCB.push_back(new_pair);
        if (reqCB.MEMORY_R) analyses_with_memRCB.push_back(new_pair);
        if (reqCB.MEMORY_W) analyses_with_memWCB.push_back(new_pair);
    }

    #define HANDLE_CALLBACK(pairs, CB, ...) \
        void const* location = __builtin_return_address(0);\
        if (isRef) visitedLocs.insert(location);\
        _Pragma("unroll(5)") for (auto it = pairs.begin(); it < pairs.end();) { \
            it = fastVisit([&](auto& analysis) { \
                Fulfillment f = analysis->CB(std::move(location), __VA_ARGS__);\
            if (f != Fulfillment::UNKNOWN) { \
                contract_status[it->formula] = f; \
                return pairs.erase(it); \
            } return ++it;}, it->analysis);\
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
            msg.msg = {std::string("Operation Message (if defined) or contract string: ") + form->msg};
            switch (form->conn) {
                case UNARY_CALL:
                case UNARY_CALLTAG: {
                    CallTagOp_t* cOP = (CallTagOp_t*)form->data;
                    msg.msg.push_back(std::string("Did not find call to ") + cOP->target_tag);
                    break;
                }
                case UNARY_RELEASE: {
                    ReleaseOp_t* rOP = (ReleaseOp_t*)form->data;
                    msg.msg.push_back(std::string("Found forbidden operation!"));
                    break;
                }
                default: return {{"UNEXPECTED OPERATION IN RESOLVE STEP"}, {}};
            }
            std::vector<void const*> const& references = analysis_references[form];
            for (void const* loc : references)
                msg.msg.push_back(std::string("Reference: ") + DynamicUtils::getFileRefStr(loc));
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
                    DynamicUtils::out() << "  Postcondition:\n";
                    formatError(errors_postcond);
                }
            }
        }
    }

    void printCoverageFile() {
        if (visitedLocs.empty()) return;
        std::srand(std::time({}) + getpid());
        std::string file_suffix = std::format("{:x}", rand());
        std::ofstream coverage_file("CoVerCoverage_" + file_suffix);
        for (void const* loc : visitedLocs) {
            std::optional<std::pair<std::string, const void *>> info = DynamicUtils::getDLInfo(loc);
            if (!info) continue;
            coverage_file << std::format("{}|{:x}", info->first, (uintptr_t)info->second) << "\n";
        }
    }

    void PPDCV_destructor() {
        #warning todo find better way for postprocessing
        std::cout << "CoVer-Dynamic: Analysis finished.\n";
        for (AnalysisPair& pair : all_analyses) {
            fastVisit([&](auto&& analysis) {
                if (!contract_status.contains(pair.formula))
                    contract_status[pair.formula] = analysis->onProgramExit(std::move(__builtin_return_address(0)));
                analysis_references[pair.formula] = analysis->getReferences();
                delete analysis;
            }, pair.analysis);
        }
        resolveContracts();
        printCoverageFile();
    }
}
