#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <ctime>
#include <variant>
#include <vector>

#include "Analyses/BaseAnalysis.h"
#include "DynamicUtils.h"

#include "Analyses/ParamAnalysis.h"
#include "Analyses/PreCallAnalysis.h"
#include "Analyses/PostCallAnalysis.h"
#include "Analyses/ReleaseAnalysis.h"
#include "DynamicAnalysis.h"
#include "FastVariant.h"

namespace {
    struct ErrorMessage {
        std::vector<std::string> msg;
        std::vector<ErrorMessage> child_msg;
    };

    std::unordered_map<void*, std::vector<Contract_t>> contrs;

    using AnalysisVariant = std::variant<ParamAnalysis*,PreCallAnalysis*,PostCallAnalysis*,ReleaseAnalysis*>;

    struct AnalysisPair {
        ContractFormula_t* formula;
        AnalysisVariant analysis;
    };

    std::unordered_set<void const*> visitedLocs;
    
    std::filesystem::path const& coverage_prefix = std::getenv("COVER_COVERAGE_FOLDER") ? std::filesystem::path(std::getenv("COVER_COVERAGE_FOLDER")) : std::filesystem::current_path();

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
        AnalysisPair new_pair = {form, new Analysis(args...)};
        all_analyses.push_back(new_pair);

        CallBacks reqCB = fastVisit([&](auto& analysis) {
            return analysis->requiredCallbacks();
        }, new_pair.analysis);
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
                if (f != Fulfillment::UNKNOWN && f != Fulfillment::INACTIVE) { \
                    if (!contract_status.contains(it->formula)) contract_status[it->formula] = f; \
                    analysis_references[it->formula] = analysis->getReferences(); \
                    validateState(it->formula); \
                    return pairs.erase(it); \
                }\
                return ++it;\
            }, it->analysis);\
        }

    void validateState(ContractFormula_t* form) {
        if (formula_parents[form] && contract_status.contains(formula_parents[form])) return; // If parent already decided return early
        if (contract_status[form] != Fulfillment::VIOLATED &&
            !(contract_status[form] == Fulfillment::FULFILLED && formula_parents[form] && formula_parents[form]->conn == XOR)) return;

        ContractFormula_t* parent = formula_parents[form];
        if (parent == nullptr && contract_status[form] == Fulfillment::VIOLATED) {
            // Top-level formula is violated, perform error output
            Contract_t* C = toplevel_to_contract[form];
            DynamicUtils::out() << "## Contract violation detected! ##\n";
            DynamicUtils::out() << "Error in contract for function \"" << C->function_name << "\":\n";
            DynamicUtils::out() << (form == C->precondition ? "Precondition:\n" : "Postcondition:\n");
            formatError(recurseCreateErrorMsg(form));
            return;
        }

        int num_fulfilled = 0;
        bool has_unknown = false;
        bool has_violated = false;
        for (int i = 0; i < parent->num_children; i++) {
            if (!contract_status.contains(&parent->children[i])) has_unknown = true;
            else if (contract_status[&parent->children[i]] == Fulfillment::FULFILLED) num_fulfilled++;
            else if (contract_status[&parent->children[i]] == Fulfillment::VIOLATED) has_violated = true;
        }
        switch (parent->conn) {
            case AND:
                if (has_violated) {
                    contract_status[parent] = Fulfillment::VIOLATED;
                    validateState(parent);
                }
                break;
            case OR:
                if (num_fulfilled) contract_status[parent] = Fulfillment::FULFILLED;
            case XOR:
                if (num_fulfilled > 1) {
                    contract_status[parent] = Fulfillment::VIOLATED;
                    validateState(parent);
                }
                if (!has_unknown && !num_fulfilled) {
                    contract_status[parent] = Fulfillment::VIOLATED;
                    validateState(parent);
                }
                break;
            default:
                __builtin_unreachable();
                DynamicUtils::out() << "Unexpected connective in state validation!\n";
        }
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
                case UNARY_PARAM:
                    if (isPre) addAnalysis<ParamAnalysis>(form, func_supplier, (ParamOp_t*)form->data);
                    else DynamicUtils::createMessage("Did not expect paramop in postcond!");
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
                #warning this should really be in the analyses themselves
                case UNARY_CALL:
                case UNARY_CALLTAG: {
                    CallTagOp_t* cOP = (CallTagOp_t*)form->data;
                    msg.msg.push_back(std::string("Did not find call to ") + cOP->target_tag);
                    break;
                }
                case UNARY_PARAM: {
                    msg.msg.push_back("Invalid param!");
                    break;
                }
                case UNARY_RELEASE: {
                    msg.msg.push_back("Found forbidden operation!");
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
                        if (found) return {{std::string("More than one child satisfied for Formula (message or contract string): ") + form->msg, "Violated children:"}, child_msg};
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
        std::stringstream file_suffix;
        file_suffix << std::hex << rand();
        std::filesystem::create_directories(coverage_prefix);
        std::string output_path = coverage_prefix / ("CoVerCoverage_" + file_suffix.str());
        DynamicUtils::out() << "Writing coverage file to " << output_path << "\n";
        std::ofstream coverage_file(output_path);
        for (void const* loc : visitedLocs) {
            std::optional<std::pair<std::string, const void *>> info = DynamicUtils::getDLInfo(loc);
            if (!info) continue;
            coverage_file << info->first << "|" << std::hex << (uintptr_t)info->second << "\n";
        }
    }

    void PPDCV_destructor() {
        for (AnalysisPair const& pair : all_analyses) {
            fastVisit([&](auto&& analysis) {
                if (!contract_status.contains(pair.formula)) {
                    contract_status[pair.formula] = analysis->onProgramExit(std::move(__builtin_return_address(0)));
                    validateState(pair.formula);
                    analysis_references[pair.formula] = analysis->getReferences();
                }
                delete analysis;
            }, pair.analysis);
        }
        DynamicUtils::out() << "Analysis finished. Writing coverage file... ";
        printCoverageFile();
        std::cerr << "Done.\n";
    }
}
