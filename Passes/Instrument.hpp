#pragma once

#include "llvm/IR/PassManager.h"
#include <llvm/IR/Constant.h>
#include <llvm/IR/GlobalVariable.h>
#include <memory>
#include "ContractManager.hpp"
#include "ContractTree.hpp"

namespace llvm {

class InstrumentPass : public PassInfoMixin<InstrumentPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        std::pair<StructType*, Constant*> createTagGlobal(Module &M); // Returns type, value
        std::pair<Constant*, int64_t> createContractsGlobal(Module& M); // Returns value, number of elems (type is ptr)
        Constant* createScopeGlobal(Module& M, std::vector<std::shared_ptr<ContractFormula>> forms); // Returns value, number of elems (type is ptr)
        Constant* createFormulaGlobal(Module& M, std::shared_ptr<ContractFormula> form);
        GlobalVariable* createConstantGlobalUnique(Module& M, Constant* C, std::string name);
        GlobalVariable* createConstantGlobal(Module& M, Constant* C, std::string name);
        PointerType* Ptr_Type; 
        IntegerType* Int_Type;
        StructType* Formula_Type;
        StructType* CallOp_Type;
        StructType* CallTagOp_Type;
        Constant* Null_Const;
        ContractManagerAnalysis::ContractDatabase* DB;
};

} // namespace llvm
