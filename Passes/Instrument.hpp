#pragma once

#include "llvm/IR/PassManager.h"
#include <llvm/IR/GlobalVariable.h>
#include <memory>
#include "ContractManager.hpp"
#include "ContractTree.hpp"

namespace llvm {

class InstrumentPass : public PassInfoMixin<InstrumentPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        std::pair<StructType*, Constant*> createTagGlobal(Module &M);
        GlobalVariable* createConstantGlobal(Module& M, Constant* C, std::string name);
        PointerType* Ptr_Type; 
        IntegerType* Int_Type;
        ContractManagerAnalysis::ContractDatabase* DB;
};

} // namespace llvm
