export module StackIntrinsics;
import std;
import LLVMModule;
import BasicTypes;

using namespace llvm;

// Announces stack frames to the dynamic analysis via CoVer_AllocStack/CoVer_FreeStack.
export class StackIntrinsicsPass : public PassInfoMixin<StackIntrinsicsPass> {
    public:
        PreservedAnalyses run(Module& M, ModuleAnalysisManager& AM) {
            BasicTypesAnalysis::BasicTypes Basic_Types = AM.getResult<BasicTypesAnalysis>(M);

            // IntrinsicsPass declared these, but optimization may have dropped them if no contract refers to them
            FunctionCallee allocStackCallee = M.getOrInsertFunction("CoVer_AllocStack", FunctionType::get(Basic_Types.Void_Type, {Basic_Types.Ptr_Type, Basic_Types.Int64_Type}, false));
            FunctionCallee freeStackCallee = M.getOrInsertFunction("CoVer_FreeStack", FunctionType::get(Basic_Types.Void_Type, {Basic_Types.Ptr_Type}, false));

            // Instrument all functions that still have allocas
            Function *FrameAddrIntrin = Intrinsic::getOrInsertDeclaration(&M, Intrinsic::frameaddress, {Basic_Types.Ptr_Type});
            Function *StackSaveIntrin = Intrinsic::getOrInsertDeclaration(&M, Intrinsic::stacksave, {Basic_Types.Ptr_Type});
            for (Function& F : M) {
                if (F.isDeclaration()) continue;
                bool hasAlloca = false;
                for (BasicBlock const& BB : F) {
                    for (Instruction const& I : BB) {
                        if (isa<AllocaInst>(&I)) {
                            hasAlloca = true;
                            break;
                        }
                    }
                }
                if (!hasAlloca) continue;

                IRBuilder<> Builder(&*F.getEntryBlock().getFirstNonPHIOrDbgOrAlloca());

                CallInst *BasePtr = Builder.CreateCall(FrameAddrIntrin, {Basic_Types.getInt(0)}, "base_ptr");
                CallInst *StackPtr = Builder.CreateCall(StackSaveIntrin, {}, "stack_ptr");

                Value *BaseInt = Builder.CreatePtrToInt(BasePtr, Basic_Types.Int64_Type, "base_int");
                Value *StackInt = Builder.CreatePtrToInt(StackPtr, Basic_Types.Int64_Type, "stack_int");

                Value *StackSize = Builder.CreateSub(BaseInt, StackInt, "stack_size");
                Builder.CreateCall(allocStackCallee, {StackPtr, StackSize});

                EscapeEnumerator EE(F, "", false);
                while (IRBuilder<>* IRB = EE.Next()) {
                    IRB->CreateCall(freeStackCallee, {StackPtr});
                }
            }

            return PreservedAnalyses::none();
        }
};
