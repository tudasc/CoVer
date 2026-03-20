#include "BasicTypes.hpp"

#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>

using namespace llvm;

BasicTypesAnalysis::BasicTypes BasicTypesAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
    BasicTypes types;

    // Basic Types
    types.Ptr_Type = PointerType::get(M.getContext(), 0);
    types.Int_Type = IntegerType::get(M.getContext(), 32);
    types.Int64_Type = IntegerType::get(M.getContext(), 64);
    types.Bool_Type = IntegerType::get(M.getContext(), 1);
    types.Void_Type = Type::getVoidTy(M.getContext());

    // Basic Constants
    types.Null_Const = ConstantPointerNull::getNullValue(types.Ptr_Type);

    return types;
}
