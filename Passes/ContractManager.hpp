#pragma once

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

        //Result Type
        struct Result {
            std::vector<Contract> Contracts;
            std::map<const Function*, std::vector<std::string>> Tags;
            #warning HACK: Never invalidate result
            bool invalidate(Module &M, const PreservedAnalyses &PA, ModuleAnalysisManager::Invalidator &) { return false; }
        } typedef ContractDatabase;

        // Run Pass
        ContractDatabase run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
