module;

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/Support/Casting.h>
#include <vector>

export module AnalysisCleanup;
import LLVMModule;

using namespace llvm;

export class AnalysisCleanupPass : public PassInfoMixin<AnalysisCleanupPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
            // Remove pseudofuncts from Fortran code
            for (Function& F : make_early_inc_range(M)) {
                if (F.getName().starts_with("contract_definitions_fort")) F.eraseFromParent();
            }

            // Remove pure dummy function calls
            std::vector<Function*> dummy_funcs = {M.getFunction("CoVer_AllocStack"), M.getFunction("CoVer_FreeStack"), M.getFunction("CoVer_RegisterGlobal")};
            for (Function* remF : dummy_funcs) {
                for (User* U : make_early_inc_range(remF->users())) {
                    if (CallBase* CB = dyn_cast<CallBase>(U)) {
                        if (CB->getCalledOperand() != remF) continue;
                        CB->eraseFromParent();
                    }
                }
            }

            return PreservedAnalyses::all();
        }
};
