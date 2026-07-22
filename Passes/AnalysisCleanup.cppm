module;

#include <vector>

export module AnalysisCleanup;
import LLVMModule;

using namespace llvm;

export class AnalysisCleanupPass : public PassInfoMixin<AnalysisCleanupPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
            // Remove pseudofuncts from Fortran code
            std::vector<Function*> to_remove;
            for (Function& F : M) {
                if (F.getName().starts_with("contract_definitions_fort")) to_remove.push_back(&F);
            }
            for (Function* F : to_remove) F->eraseFromParent();

            return PreservedAnalyses::all();
        }
};
