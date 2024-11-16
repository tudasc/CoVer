#pragma once

#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include "llvm/IR/PassManager.h"
#include <llvm/IR/InstrTypes.h>
#include <map>
#include <set>

namespace llvm {

class ContractVerifierPreCallPass : public PassInfoMixin<ContractVerifierPreCallPass> {
    public:
        enum struct CallStatusVal { CALLED, NOTCALLED, PARAMCHECK, ERROR };
        struct CallStatus {
            CallStatusVal CurVal;
            std::set<const CallBase*> candidate;
        };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static std::string createDebugStr(const CallBase* Provider, const std::set<const CallBase *> candidates);

    private:
        CallStatusVal checkPreCall(const ContractTree::CallOperation& cOP, const ContractManagerAnalysis::Contract& C, const bool isTag, const Module& M, std::string& error);
        std::map<const Function*, std::vector<TagUnit>> Tags;
};

} // namespace llvm
