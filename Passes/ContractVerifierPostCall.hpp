#pragma once

#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include "llvm/IR/PassManager.h"
#include <llvm/IR/InstrTypes.h>
#include <map>

namespace llvm {

class ContractVerifierPostCallPass : public PassInfoMixin<ContractVerifierPostCallPass> {
    public:
        enum struct CallStatus { CALLED, NOTCALLED };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static std::string createDebugStr(const CallBase* Provider);

    private:
        CallStatus checkPostCall(const ContractTree::CallOperation* cOP, const ContractManagerAnalysis::LinearizedContract& C, const bool isTag, const Module& M, std::string& error);
        std::map<const Function*, std::vector<TagUnit>> Tags;
};

} // namespace llvm
