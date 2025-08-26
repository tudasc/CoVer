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

#include "Analyses/BaseAnalysis.h"
#include "DynamicUtils.h"
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

    std::unordered_set<void const*> visitedLocs;

    std::unordered_map<ContractFormula_t*, Fulfillment> contract_status;
    std::unordered_map<ContractFormula_t*, ContractFormula_t*> formula_parents;
    std::unordered_map<ContractFormula_t*, Contract_t*> toplevel_to_contract;
    std::vector<AnalysisPair> all_analyses;
    std::vector<AnalysisPair> analyses_with_funcCB;
    std::vector<AnalysisPair> analyses_with_memRCB;
    std::vector<AnalysisPair> analyses_with_memWCB;
    std::unordered_map<ContractFormula_t*, std::vector<void const*>> analysis_references;

    ErrorMessage recurseCreateErrorMsg(ContractFormula_t* form);
    void formatError(ErrorMessage msg, int indent = 2);

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
                analysis_references[it->formula] = analysis->getReferences(); \
                validateState(it->formula); \
                return pairs.erase(it); \
            } return ++it;}, it->analysis);\
        }

    void validateState(ContractFormula_t* form) {
        if (contract_status[form] != Fulfillment::VIOLATED) return;
        if (contract_status.contains(formula_parents[form])) return;

        ContractFormula_t* parent = formula_parents[form];
        if (parent == nullptr) {
            // Top-level formula is violated, perform error output
            Contract_t* C = toplevel_to_contract[form];
            DynamicUtils::out() << "Error in contract for function \"" << C->function_name << "\":\n";
            DynamicUtils::out() << (form == C->precondition ? "Precondition:\n" : "Postcondition:\n");
            formatError(recurseCreateErrorMsg(form));
            return;
        }

        switch (parent->conn) {
            case AND:
                // Failure guaranteed, parent is AND and has a violated member
                contract_status[parent] = Fulfillment::VIOLATED;
                validateState(parent);
                return;
            case OR:
            case XOR: {
                int num_fulfilled = 0;
                for (int i = 0; i < parent->num_children; i++) {
                    if (contract_status.contains(&parent->children[i]) && contract_status[&parent->children[i]] == Fulfillment::FULFILLED) {
                        num_fulfilled++;
                    }
                }
                if (parent->conn == OR && num_fulfilled > 0) { // Early fulfill OR if at least one satisfied
                    contract_status[parent] = Fulfillment::FULFILLED;
                    validateState(parent);
                } else if (parent->conn == XOR && num_fulfilled > 1) { // Early violate XOR if more than one satisfied
                    contract_status[parent] = Fulfillment::VIOLATED;
                    validateState(parent);
                }
                return;
            }
            default:
                __builtin_unreachable();
                DynamicUtils::out() << "Unexpected connective in state validation!\n";
        }
        // If this is reached: No new information gained this time.
    }

    void recurseCreateAnalyses(ContractFormula_t* form, ContractFormula_t* parent, bool isPre, void* func_supplier) {
        formula_parents[form] = parent;
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
            for (int i = 0; i < form->num_children; i++) recurseCreateAnalyses(&form->children[i], form, isPre, func_supplier);
        }
    }

    ErrorMessage recurseCreateErrorMsg(ContractFormula_t* form) {
        if (contract_status[form] != Fulfillment::VIOLATED) return {};
        if (form->num_children == 0) {
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
                default: __builtin_unreachable();
            }
            std::vector<void const*> const& references = analysis_references[form];
            for (void const* loc : references)
                msg.msg.push_back(std::string("Reference: ") + DynamicUtils::getFileRefStr(loc));
            return msg;
        }
        std::vector<ErrorMessage> child_msg;
        for (int i = 0; i < form->num_children; i++) {
            child_msg.push_back(recurseCreateErrorMsg(&form->children[i]));
        }
        switch (form->conn) {
            case AND:
                return {{std::string("A child is not satisfied for Formula (message or contract string): ") + form->msg}, child_msg};
            case OR:
                return {{std::string("No child satisfied for Formula (message or contract string): ") + form->msg}, child_msg};
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
            default: __builtin_unreachable();
        }
    }

    void formatError(ErrorMessage msg, int indent) {
        if (msg.msg.empty()) return;
        std::string indent_s(indent, ' ');
        for (std::string const& line : msg.msg)
            DynamicUtils::out() << indent_s << "- " << line << "\n";
        for (ErrorMessage child : msg.child_msg) {
            formatError(child, indent + 2);
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
        for (AnalysisPair const& pair : all_analyses) {
            fastVisit([&](auto&& analysis) {
                if (!contract_status.contains(pair.formula)) {
                    contract_status[pair.formula] = analysis->onProgramExit(std::move(__builtin_return_address(0)));
                    if (contract_status[pair.formula] == Fulfillment::VIOLATED) validateState(pair.formula);
                }
                analysis_references[pair.formula] = analysis->getReferences();
                delete analysis;
            }, pair.analysis);
        }
        DynamicUtils::out() << "Analysis finished. Writing coverage file... ";
        printCoverageFile();
        std::cerr << "Done.\n";
    }
}
