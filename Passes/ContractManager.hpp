#pragma once

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/PassManager.h"
#include <optional>

#include "../LangCode/ContractLangErrorListener.hpp"

#include "../LangCode/ContractDataVisitor.hpp"

namespace llvm {

class ContractManagerAnalysis : public AnalysisInfoMixin<ContractManagerAnalysis> {
    public:
        static llvm::AnalysisKey Key;

        enum Fulfillment { UNKNOWN, FULFILLED, BROKEN };

        struct Contract {
            const Function* const F;
            const StringRef ContractString;
            const ContractTree::ContractData Data;
            Fulfillment Status = UNKNOWN;
        };

        //Result Type
        struct Result {
            std::vector<Contract> Contracts;
            #warning HACK: Never invalidate result
            bool invalidate(Module &M, const PreservedAnalyses &PA, ModuleAnalysisManager::Invalidator &) { return false; }
        } typedef ContractDatabase;

        // Run Pass
        ContractDatabase run(Module &M, ModuleAnalysisManager &AM);


    private:
        ContractLangErrorListener listener;
        ContractDataVisitor dataVisitor;
};

} // namespace llvm
