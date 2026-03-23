#include "Intrinsics.hpp"
#include "BasicTypes.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <llvm/Transforms/Utils/EscapeEnumerator.h>
#include <ranges>
#include <vector>

using namespace llvm;

PreservedAnalyses IntrinsicsPass::run(Module &M, ModuleAnalysisManager &AM) {
    Basic_Types = AM.getResult<BasicTypesAnalysis>(M);
    createCallees(M);
    instrumentIntrinsics(M);
    return PreservedAnalyses::all();
}

void IntrinsicsPass::createCallees(Module& M) {
    // Get callee for alloca instr
    FunctionType* FunctionAllocStackType = FunctionType::get(Basic_Types.Void_Type, {Basic_Types.Ptr_Type}, false);
    allocStackCallee = calleeHelper(M, "CoVer_AllocStack", FunctionAllocStackType);

    FunctionType* FunctionFreeStackType = FunctionType::get(Basic_Types.Void_Type, {Basic_Types.Ptr_Type}, false);
    freeStackCallee = calleeHelper(M, "CoVer_FreeStack", FunctionFreeStackType);

    // Get callee for global vals
    FunctionType* FunctionGlobalRegType = FunctionType::get(Basic_Types.Void_Type, {Basic_Types.Ptr_Type, Basic_Types.Int64_Type}, false);
    globalRegCallee = calleeHelper(M, "CoVer_RegisterGlobal", FunctionGlobalRegType);

    // Get callee for fort intrinsics
    FunctionType* FunctionFAllocIntrinsicType = FunctionType::get(Basic_Types.Int_Type, {Basic_Types.Ptr_Type, Basic_Types.Int64_Type, Basic_Types.Int_Type}, true);
    fallocPointerCallee = calleeHelper(M, "CoVer_FPointerAllocate", FunctionFAllocIntrinsicType);

    FunctionType* FunctionFDeallocIntrinsicType = FunctionType::get(Basic_Types.Int_Type, {Basic_Types.Ptr_Type, Basic_Types.Bool_Type, Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type}, false);
    fdeallocPointerCallee = calleeHelper(M, "CoVer_FPointerDeallocate", FunctionFDeallocIntrinsicType);
}

void IntrinsicsPass::instrumentIntrinsics(Module& M) {
    // Instrument all allocas
    for (Function& F : M) {
        std::vector<AllocaInst*> stack_vars;
        for (BasicBlock& BB : F) {
            for (Instruction& I : BB) {
                if (AllocaInst* AI = dyn_cast<AllocaInst>(&I)) {
                    stack_vars.push_back(AI);
                    CallInst* intrinsicCI = CallInst::Create(allocStackCallee, {AI});
                    intrinsicCI->setDebugLoc(I.getDebugLoc());
                    intrinsicCI->insertAfter(AI);
                }
            }
        }
        EscapeEnumerator EE(F, "", false);
        while (IRBuilder<>* IRB = EE.Next()) {
            for (AllocaInst* AI : stack_vars) {
                IRB->CreateCall(freeStackCallee, {AI});
            }
        }
    }

    // Instrument global vars
    if (M.getFunction("main")) {
        auto Entry = M.getFunction("main")->getEntryBlock().getFirstNonPHIOrDbgOrAlloca();
        for (GlobalVariable& GV : M.globals()) {
            if (!GV.hasInitializer()) continue;
            if (GV.hasPrivateLinkage() || GV.hasComdat()) continue;
            if (GV.getName().starts_with("llvm.") || GV.getName().starts_with("ContractValueInfo")) continue;
            size_t gv_size = GV.getParent()->getDataLayout().getTypeAllocSize(GV.getInitializer()->getType());
            CallInst::Create(globalRegCallee, {&GV, Basic_Types.getInt64(gv_size)}, "", Entry);
        }
    }

    // Instrument Fortran allocate()
    Function* fortAllocate = M.getFunction("_FortranAPointerAllocate");
    if (fortAllocate) {
        for (User* U : fortAllocate->users()) {
            if (CallBase* CB = dyn_cast<CallBase>(U)) {
                // Need to figure out the size.
                struct DimInfo {
                    std::vector<Value*> params;
                    Value* size;
                };
                std::vector<DimInfo> dim_params;
                CallBase* cur = dyn_cast<CallBase>(CB->getPrevNode());
                while (cur) {
                    if (cur->getCalledOperand() != M.getFunction("_FortranAPointerSetBounds")) break;
                    Value* lower = cur->getArgOperand(2);
                    Value* upper = cur->getArgOperand(3);
                    Value* one = Basic_Types.getInt64(1);
                    Instruction* diff = BinaryOperator::CreateSub(upper, lower, "", CB->getIterator());
                    Instruction* res = BinaryOperator::CreateAdd(diff, one, "", CB->getIterator());
                    std::vector<Value*> args;
                    // Skip 0, which is just the pointer
                    args.push_back(cur->getArgOperand(1));
                    args.push_back(cur->getArgOperand(2));
                    args.push_back(cur->getArgOperand(3));
                    dim_params.push_back({args, res});
                    CallBase* prev = cur;
                    cur = cur->getPrevNode() ? dyn_cast<CallBase>(cur->getPrevNode()) : nullptr;
                    prev->eraseFromParent();
                }
                Value* total_elems = Basic_Types.getInt64(1);
                for (DimInfo const& info : dim_params) {
                    total_elems = BinaryOperator::CreateMul(total_elems, info.size, "", CB->getIterator());
                }
                // Finally, multiply by base type size
                // First, get base type size from global descriptor
                GetElementPtrInst* GEP = GetElementPtrInst::Create(CB->getArgOperand(0)->getType(), CB->getArgOperand(0), {Basic_Types.getInt(1)});
                GEP->insertBefore(CB->getIterator());
                LoadInst* BaseSize = new LoadInst(Basic_Types.Int64_Type, GEP, "", false, CB->getIterator());
                total_elems = BinaryOperator::CreateMul(total_elems, BaseSize, "", CB->getIterator());
                std::vector<Value*> intrinsicparams = {CB->getArgOperand(0), total_elems, Basic_Types.getInt(dim_params.size())};
                std::ranges::reverse_view rv{dim_params}; // Reverse to set bounds in correct order
                for (DimInfo const& info : rv) {
                    for (Value* arg : info.params) intrinsicparams.push_back(arg);
                }

                intrinsicparams.push_back(CB->getArgOperand(1));
                intrinsicparams.push_back(CB->getArgOperand(2));
                intrinsicparams.push_back(CB->getArgOperand(3));
                intrinsicparams.push_back(CB->getArgOperand(4));
                CallInst* intrinsicCI = CallInst::Create(fallocPointerCallee, intrinsicparams);
                ReplaceInstWithInst(CB, intrinsicCI);
            }
        }
    }

    // Instrument Fortran deallocate()
    Function* fortDeallocate = M.getFunction("_FortranAPointerDeallocate");
    if (fortDeallocate) {
        for (User* U : fortDeallocate->users()) {
            if (CallBase* CB = dyn_cast<CallBase>(U)) {
                if (CB->getCalledOperand() != fortDeallocate) continue;
                CB->setCalledFunction(fdeallocPointerCallee);
            }
        }
    }
}

FunctionCallee IntrinsicsPass::calleeHelper(Module& M, std::string name, FunctionType* fnType) {
    AttributeList fnAttr;
    fnAttr = fnAttr.addFnAttribute(M.getContext(), Attribute::NoUnwind);
    fnAttr = fnAttr.addFnAttribute(M.getContext(), Attribute::WillReturn);
    fnAttr = fnAttr.addFnAttribute(M.getContext(), Attribute::NoCallback);

    FunctionCallee res = M.getOrInsertFunction(name + "_TMP", fnType, fnAttr);
    if (M.getFunction(name)) {
        Function* old = M.getFunction(name);
        old->replaceAllUsesWith(res.getCallee());
        old->eraseFromParent();
    }
    res.getCallee()->setName(name);

    // Create debug info
    DIBuilder DIB(M);
    SmallVector<Metadata*, 4> MDTypes;
    for (Type const* T : fnType->subtypes()) {
        MDTypes.push_back(Basic_Types.getMDForType(T));
    }
    DISubroutineType* SRT = DIB.createSubroutineType(DIB.getOrCreateTypeArray(MDTypes));
    DISubprogram* SP = DIB.createFunction(*M.debug_compile_units_begin(), name, name, M.debug_compile_units_begin()->getFile(), 0, SRT, 0);

    // Finalize
    Function* F = dyn_cast<Function>(res.getCallee());
    F->setLinkage(GlobalValue::ExternalLinkage);
    F->setSubprogram(SP);

    DIB.finalize();
    return res;
}
