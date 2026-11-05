#pragma once

#include <json/value.h>

#include "ContractTree.hpp"

import std;
import LLVMModule;
import ErrorMessage;
import AnalysisReports;

using namespace ContractTree;

namespace llvm {

class ContractManagerAnalysis : public AnalysisInfoMixin<ContractManagerAnalysis> {
    public:
        static inline llvm::AnalysisKey Key;

        // Shared with the interactive mode, see AnalysisReports
        using FormulaReports = AnalysisReports::FormulaReports;
        using Contract = AnalysisReports::Contract;
        using DebugContext = AnalysisReports::DebugContext;
        using FormulaDebugHandlers = AnalysisReports::FormulaDebugHandlers;

        struct LinearizedContract {
            Function* F;
            const std::string ContractString;
            const std::vector<std::shared_ptr<ContractExpression>> Pre;
            const std::vector<std::shared_ptr<ContractExpression>> Post;
            std::shared_ptr<std::vector<std::string>> DebugInfo;
        };

        //Result Type
        struct Result {
            FormulaReports reports;
            FormulaDebugHandlers debugHandlers;
            std::vector<Contract> Contracts; // For postprocessing only
            std::vector<LinearizedContract> LinearizedContracts; // For verification passes
            std::map<Function*, std::vector<TagUnit>> Tags;
            std::map<std::string, std::set<Value*>> ContractVariableData;
            std::chrono::time_point<std::chrono::system_clock> start_time;
            bool allowMultiReports = false;
            Json::Value processedReports;
            bool invalidate(Module &, PreservedAnalyses const&, ModuleAnalysisManager::Invalidator const&) const {
                return false;
            }
            bool isInteractive;
        } typedef ContractDatabase;

        // Run Analysis
        ContractDatabase run(Module &M, ModuleAnalysisManager &AM);

    private:
        ContractDatabase curDatabase;
        const std::vector<std::shared_ptr<ContractExpression>> linearizeContractFormula(const std::shared_ptr<ContractFormula> contrF);

        void extractFromAnnotations(const Module& M); // C/C++ Attributes
        void extractFromFunction(Module& M); // Fortran Workaround

        void addContract(std::string contract, Function* F);
        void addValueDefinition(std::string name, Value* val);
};

} // namespace llvm
