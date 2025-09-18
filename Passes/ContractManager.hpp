#pragma once

#include <chrono>
#include <json/value.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/CommandLine.h>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ContractTree.hpp"

using namespace ContractTree;

namespace llvm {

class ContractManagerAnalysis : public AnalysisInfoMixin<ContractManagerAnalysis> {
    public:
        static inline llvm::AnalysisKey Key;

        struct Contract {
            Function* F;
            const std::string ContractString;
            const ContractData Data;
            std::shared_ptr<std::vector<std::string>> DebugInfo = std::make_shared<std::vector<std::string>>();
        };
        struct LinearizedContract {
            Function* F;
            const std::string ContractString;
            const std::vector<std::shared_ptr<ContractExpression>> Pre;
            const std::vector<std::shared_ptr<ContractExpression>> Post;
            std::shared_ptr<std::vector<std::string>> DebugInfo;
        };

        //Result Type
        struct Result {
            std::vector<Contract> Contracts; // For postprocessing only
            std::vector<LinearizedContract> LinearizedContracts; // For verification passes
            std::map<Function*, std::vector<TagUnit>> Tags;
            std::map<std::string, Value*> ContractVariableData;
            std::chrono::time_point<std::chrono::system_clock> start_time;
            bool allowMultiReports = false;
            Json::Value processedReports;
            bool invalidate(Module &, PreservedAnalyses const&, ModuleAnalysisManager::Invalidator const&) const {
                return false;
            }
        } typedef ContractDatabase;

        // Run Pass
        ContractDatabase run(Module &M, ModuleAnalysisManager &AM);

    private:
        ContractDatabase curDatabase;
        const std::vector<std::shared_ptr<ContractExpression>> linearizeContractFormula(const std::shared_ptr<ContractFormula> contrF);

        void extractFromAnnotations(const Module& M); // C/C++ Attributes
        void extractFromFunction(Module& M); // Fortran Workaround

        void addContract(std::string contract, Function* F);
};

} // namespace llvm
