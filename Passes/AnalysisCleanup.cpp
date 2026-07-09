#include "AnalysisCleanup.hpp"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

PreservedAnalyses AnalysisCleanupPass::run(Module &M, ModuleAnalysisManager &AM) {
    // Remove pseudofuncts from Fortran code
    std::vector<Function*> to_remove;
    for (Function& F : M) {
        if (F.getName().starts_with("contract_definitions_fort")) to_remove.push_back(&F);
    }
    for (Function* F : to_remove) F->eraseFromParent();

    return PreservedAnalyses::all();
}
