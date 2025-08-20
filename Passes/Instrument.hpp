#pragma once

#include "llvm/IR/PassManager.h"
#include <llvm/IR/Constant.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <memory>
#include <set>
#include <unordered_set>
#include <vector>
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "ErrorMessage.h"

namespace llvm {

class InstrumentPass : public PassInfoMixin<InstrumentPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        // Structure Creation
        Constant* createTagGlobal(Module &M); // Returns type, value
        std::pair<Constant*, int64_t> createReferencesGlobal(Module &M);
        std::pair<Constant*, int64_t> createContractsGlobal(Module& M); // Returns value, number of elems (type is ptr)
        Constant* createScopeGlobal(Module& M, std::vector<std::shared_ptr<ContractFormula>> forms); // Returns value, number of elems (type is ptr)
        Constant* createFormulaGlobal(Module& M, std::shared_ptr<ContractFormula> form);
        Constant* createOperationGlobal(Module& M, std::shared_ptr<const Operation> op);
        std::pair<Constant*,int64_t> createParamList(Module& M, std::vector<CallParam> p);

        // Auxiliary
        GlobalVariable* createConstantGlobalUnique(Module& M, Constant* C, std::string name);
        GlobalVariable* createConstantGlobal(Module& M, Constant* C, std::string name);
        void createTypes(Module& M);

        // Function Instrumentation
        void instrumentFunctions(Module &M);
        void instrumentRW(Module &M);
        void insertFunctionInstrCallback(Function* CB);
        void insertCBIfNeeded(FunctionCallee FC, std::vector<Value *> params, Instruction* I);
        bool isRelevant(Instruction const* I);
        FunctionCallee callbackFuncCallee;
        FunctionCallee callbackRCallee;
        FunctionCallee callbackWCallee;
        std::set<Function*> already_instrumented;

        // Types
        PointerType* Ptr_Type;
        IntegerType* Bool_Type;
        IntegerType* Int_Type;
        Type* Void_Type;
        StructType* Formula_Type;
        StructType* DB_Type;
        StructType* Tag_Type;
        StructType* Tags_Type;
        StructType* Param_Type;
        StructType* CallOp_Type;
        StructType* CallTagOp_Type;
        StructType* ReleaseOp_Type;
        StructType* RWOp_Type;
        StructType* Contract_Type;
        Constant* Null_Const;

        std::unordered_set<FileReference> references;
        std::unordered_set<Instruction*> instrument_ignore;

        ContractManagerAnalysis::ContractDatabase* DB;
};

} // namespace llvm
