#include "Instrument.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Compiler.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/WithColor.h>
#include <utility>
#include <vector>


using namespace llvm;
using namespace ContractTree;

PreservedAnalyses InstrumentPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    DB = &AM.getResult<ContractManagerAnalysis>(M);

    Function* mainF = M.getFunction("main");
    if (!mainF) return PreservedAnalyses::all(); // No point

    // Generic Types
    Ptr_Type = PointerType::get(M.getContext(), 0);
    Int_Type = IntegerType::get(M.getContext(), 64);

    // Create Tag globals
    StructType* TagType;
    Constant* TagVal;
    std::tie(TagType, TagVal) = createTagGlobal(M);

    // Package database
    StructType* DB_Type = StructType::create(M.getContext(), "ContractDB_t");
    DB_Type->setBody( {TagType, TagType} );
    GlobalVariable* GlobalDB = dyn_cast<GlobalVariable>(M.getOrInsertGlobal("ContractDB", DB_Type));
    Constant* CDB = ConstantStruct::get(DB_Type, {TagVal, TagVal});
    GlobalDB->setInitializer(CDB);

    // Create initialization routine for tool
    Type* T = Type::getVoidTy(M.getContext());
    FunctionCallee initFuncCallee = M.getOrInsertFunction("PPDCV_Initialize", T, Ptr_Type);
    Function* initFunc = dyn_cast<Function>(initFuncCallee.getCallee());
    initFunc->setLinkage(GlobalValue::ExternalLinkage);
    CallInst* initFuncCI = CallInst::Create(initFuncCallee, GlobalDB);
    Instruction* entryI = mainF->getEntryBlock().getFirstNonPHIOrDbg();
    initFuncCI->insertBefore(entryI);

    return PreservedAnalyses::all();
}

std::pair<StructType*, Constant*> InstrumentPass::createTagGlobal(Module& M) {
    // Tag type: 
    StructType* TagTy = StructType::create(M.getContext(), "Tag_t");
    TagTy->setBody({Ptr_Type, Int_Type});

    // Create Tags
    std::vector<Constant*> tags;
    std::vector<Constant*> funcs;
    int count = 0;
    for (std::pair<Function*, std::vector<TagUnit>> functags : DB->Tags) {
        for (TagUnit tag : functags.second) {
            Constant* param = ConstantInt::get(Int_Type, tag.param ? *tag.param : -1);
            Constant* str = ConstantDataArray::getString(M.getContext(), tag.tag);
            GlobalVariable* strGlobal = createConstantGlobal(M, str, "CONTR_TAG_STR_" + tag.tag);
            Constant* TagC = ConstantStruct::get(TagTy, {strGlobal,param});
            funcs.push_back(functags.first);
            tags.push_back(TagC);
            count++;
        }
    }

    StructType* TagsTy = StructType::create(M.getContext(), "TagsMap_t");

    // Create global const arrays for the tags
    ArrayType* ArrFuncTy = ArrayType::get(Ptr_Type, count);
    ArrayType* ArrTagTy = ArrayType::get(TagTy, count);
    GlobalVariable* ptrFuncs = createConstantGlobal(M, ConstantArray::get(ArrFuncTy, funcs), "CONTR_TAG_ARRAY_PTRS");
    GlobalVariable* ptrTags = createConstantGlobal(M, ConstantArray::get(ArrTagTy, tags), "CONTR_TAG_ARRAY_TAGS");

    TagsTy->setBody(
        {Ptr_Type, Ptr_Type, Int_Type}
    );

    Constant* TagsStruct = ConstantStruct::get(TagsTy, {ptrFuncs, ptrTags, ConstantInt::get(Int_Type, count)});
    return {TagsTy, TagsStruct};
}

GlobalVariable* InstrumentPass::createConstantGlobal(Module& M, Constant* C, std::string name) {
    GlobalVariable* GV = dyn_cast<GlobalVariable>(M.getOrInsertGlobal(name, C->getType()));
    GV->setInitializer(C);
    return GV;
}
