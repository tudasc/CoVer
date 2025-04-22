#pragma once

#include <chrono>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Function.h>
#include <map>
#include <memory>
#include <vector>

#include "ContractTree.hpp"

using namespace ContractTree;

namespace llvm {

class ContractManagerAnalysis : public AnalysisInfoMixin<ContractManagerAnalysis> {
    public:
        static llvm::AnalysisKey Key;

        struct Contract {
            const Function* const F;
            const StringRef ContractString;
            const ContractData Data;
            std::shared_ptr<std::vector<std::string>> DebugInfo = std::make_shared<std::vector<std::string>>();
        };
        struct LinearizedContract {
            const Function* const F;
            const StringRef ContractString;
            const std::vector<std::shared_ptr<ContractExpression>> Pre;
            const std::vector<std::shared_ptr<ContractExpression>> Post;
            std::shared_ptr<std::vector<std::string>> DebugInfo;
        };

        //Result Type
        struct Result {
            std::vector<Contract> Contracts; // For postprocessing only
            std::vector<LinearizedContract> LinearizedContracts; // For verification passes
            std::map<const Function*, std::vector<TagUnit>> Tags;
            std::chrono::time_point<std::chrono::system_clock> start_time;
        } typedef ContractDatabase;

        // Run Pass
        ContractDatabase run(Module &M, ModuleAnalysisManager &AM);
    
    private:
        ContractDatabase curDatabase;
        const std::vector<std::shared_ptr<ContractExpression>> linearizeContractFormula(const std::shared_ptr<ContractFormula> contrF);

        void extractFromAnnotations(const Module& M); // C/C++ Attributes
        void extractFromFunction(const Module& M); // Fortran Workaround

        void addContract(StringRef contract, Function* F);
};

} // namespace llvm
