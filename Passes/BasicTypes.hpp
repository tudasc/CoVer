#pragma once

#include <llvm/IR/Analysis.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/PassManager.h>

namespace llvm {

class BasicTypesAnalysis : public AnalysisInfoMixin<BasicTypesAnalysis> {
    public:
        static inline llvm::AnalysisKey Key;
        //Result Type
        struct Result {
            PointerType* Ptr_Type;
            IntegerType* Bool_Type;
            Constant* getBool(bool x) { return ConstantInt::getBool(Bool_Type, x); }
            IntegerType* Int_Type;
            ConstantInt* getInt(int x) { return ConstantInt::get(Int_Type, x); }
            IntegerType* Int64_Type;
            ConstantInt* getInt64(int x) { return ConstantInt::get(Int64_Type, x); }
            Type* Void_Type;
            Constant* Null_Const;
            bool invalidate(Module &, PreservedAnalyses const&, ModuleAnalysisManager::Invalidator const&) const {
                return false;
            }
        } typedef BasicTypes;
        // Run Analysis
        BasicTypes run(Module &M, ModuleAnalysisManager &AM);
};

}
