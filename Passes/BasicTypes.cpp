#include "BasicTypes.hpp"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>

using namespace llvm;

BasicTypesAnalysis::BasicTypes BasicTypesAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
    BasicTypes types;

    DIBuilder DIB(M);

    // Basic Types
    types.Ptr_Type = PointerType::get(M.getContext(), 0);
    types.TypeToMD[types.Ptr_Type] = DIB.createBasicType("void*", 64, dwarf::DW_TAG_pointer_type);

    types.Int_Type = IntegerType::get(M.getContext(), 32);
    types.TypeToMD[types.Int_Type] = DIB.createBasicType("int32_t", 32, dwarf::DW_ATE_signed);

    types.Int64_Type = IntegerType::get(M.getContext(), 64);
    types.TypeToMD[types.Int64_Type] = DIB.createBasicType("int64_t", 64, dwarf::DW_ATE_signed);

    types.Bool_Type = IntegerType::get(M.getContext(), 1);
    types.TypeToMD[types.Bool_Type] = DIB.createBasicType("bool", 1, dwarf::DW_ATE_boolean);

    types.Void_Type = Type::getVoidTy(M.getContext());
    types.TypeToMD[types.Void_Type] = nullptr;

    // Basic Constants
    types.Null_Const = ConstantPointerNull::getNullValue(types.Ptr_Type);

    DIB.finalize();

    return types;
}
