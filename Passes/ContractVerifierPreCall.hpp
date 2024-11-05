#pragma once

#include "ContractTree.hpp"
#include "llvm/IR/PassManager.h"
#include <llvm/IR/InstrTypes.h>
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

    private:
        CallStatusVal checkPreCall(const ContractTree::CallOperation& cOP, const Function* F, const Module& M, std::string& error);
};

} // namespace llvm
