#pragma once

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/PassManager.h>
#include <memory>

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
            std::shared_ptr<Fulfillment> Status = std::make_shared<Fulfillment>(Fulfillment::UNKNOWN);
        };

        //Result Type
        struct Result {
            std::vector<Contract> Contracts;
            #warning HACK: Never invalidate result
            bool invalidate(Module &M, const PreservedAnalyses &PA, ModuleAnalysisManager::Invalidator &) { return false; }
        } typedef ContractDatabase;

        // Run Pass
        ContractDatabase run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
