#include "Instrument.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include <functional>
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
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace ContractTree;

PreservedAnalyses InstrumentPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    DB = &AM.getResult<ContractManagerAnalysis>(M);

    Function* mainF = M.getFunction("main");
    if (!mainF) return PreservedAnalyses::all(); // No point

    // Generic Types and consts
    createTypes(M);

    // Create Tag globals
    StructType* TagType;
    Constant* TagVal;
    std::tie(TagType, TagVal) = createTagGlobal(M);

    // Create Contract globals
    Constant* ContractsVal;
    uint64_t num_contrs;
    std::tie(ContractsVal, num_contrs) = createContractsGlobal(M);

    // Package database
    StructType* DB_Type = StructType::create(M.getContext(), "ContractDB_t");
    DB_Type->setBody( {Ptr_Type, Int_Type, TagType} );
    GlobalVariable* GlobalDB = dyn_cast<GlobalVariable>(M.getOrInsertGlobal("CONTR_DB", DB_Type));
    Constant* CDB = ConstantStruct::get(DB_Type, {ContractsVal, ConstantInt::get(Int_Type, num_contrs),  TagVal});
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

    // Create global const arrays for the tags
    ArrayType* ArrFuncTy = ArrayType::get(Ptr_Type, count);
    ArrayType* ArrTagTy = ArrayType::get(TagTy, count);
    GlobalVariable* ptrFuncs = createConstantGlobal(M, ConstantArray::get(ArrFuncTy, funcs), "CONTR_TAG_ARRAY_PTRS");
    GlobalVariable* ptrTags = createConstantGlobal(M, ConstantArray::get(ArrTagTy, tags), "CONTR_TAG_ARRAY_TAGS");

    // Full tag map structure
    StructType* TagsTy = StructType::create(M.getContext(), "TagsMap_t");
    TagsTy->setBody(
        {Ptr_Type, Ptr_Type, Int_Type}
    );

    Constant* TagsStruct = ConstantStruct::get(TagsTy, {ptrFuncs, ptrTags, ConstantInt::get(Int_Type, count)});
    return {TagsTy, TagsStruct};
}

std::pair<Constant*, int64_t> InstrumentPass::createContractsGlobal(Module& M) {
    std::vector<Constant*> contractConsts;
    for (ContractManagerAnalysis::Contract C : DB->Contracts) {
        if (C.Data.Pre.empty() && C.Data.Post.empty()) continue;
        Constant* PrecondConst = createScopeGlobal(M, C.Data.Pre);
        Constant* PostcondConst = createScopeGlobal(M, C.Data.Post);
        Constant* funcStr = ConstantDataArray::getString(M.getContext(), C.F->getName());
        GlobalVariable* strGlobal = createConstantGlobal(M, funcStr, "CONTR_FUNC_STR_" + C.F->getName().str());
        Constant* contr = ConstantStruct::get(Contract_Type, {PrecondConst, PostcondConst, C.F, strGlobal});
        contractConsts.push_back(contr);
    }

    // Create list of contracts
    ArrayType* ArrContracts = ArrayType::get(Contract_Type, contractConsts.size());
    GlobalVariable* arrContractGlobal = createConstantGlobal(M,  ConstantArray::get(ArrContracts, contractConsts), "CONTR_LIST_CONTRACTS");

    return {arrContractGlobal, contractConsts.size()};
}

Constant* InstrumentPass::createScopeGlobal(Module& M, std::vector<std::shared_ptr<ContractFormula>> forms) {
    std::vector<Constant*> formsConst;
    if (forms.empty()) return Null_Const;
    for (std::shared_ptr<ContractFormula> form : forms) {
        formsConst.push_back(createFormulaGlobal(M, form));
    }
    ArrayType* ArrPreCond = ArrayType::get(Formula_Type, forms.size());
    GlobalVariable* Sublevel = createConstantGlobalUnique(M, ConstantArray::get(ArrPreCond, formsConst), std::string("CONTR_SCOPECONDITIONS"));
    return createConstantGlobalUnique(M, ConstantStruct::get(Formula_Type, { Sublevel, ConstantInt::get(Int_Type, forms.size()), ConstantInt::get(Int_Type, (int64_t)FormulaType::AND), Null_Const}), "CONTR_SCOPE");
}

Constant* InstrumentPass::createFormulaGlobal(Module& M, std::shared_ptr<ContractFormula> form) {
    Constant* data = Null_Const;
    Constant* children = Null_Const;
    int64_t connective;
    if (form->Children.empty()) {
        // Expression
        std::shared_ptr<const Operation> OP = dynamic_pointer_cast<ContractExpression>(form)->OP;
        connective = (int64_t)OP->type();
        switch (OP->type()) {
            case OperationType::READ:
            case OperationType::WRITE:
                errs() << "TODO do not expect memory access in formula" << "\n";
                data = Null_Const;
            case OperationType::CALL:
                {
                    Function* F = M.getFunction(dynamic_pointer_cast<const CallOperation>(OP)->Function);
                    if (!F) errs() << "Specified function \"" << dynamic_pointer_cast<const CallOperation>(OP)->Function << "\" does not exist! Instrumentation failed!\n";
                    Constant* funcStr = ConstantDataArray::getString(M.getContext(), F->getName());
                    data = ConstantStruct::get(CallOp_Type, {F, createConstantGlobal(M, funcStr, "CONTR_FUNC_STR_" + F->getName().str())});
                    data = createConstantGlobalUnique(M, data, "CONTR_CALLOP");
                }
                break;
            case OperationType::CALLTAG:
                data = createConstantGlobal(M, ConstantDataArray::getString(M.getContext(), dynamic_pointer_cast<const CallTagOperation>(OP)->Function),
                                        "CONTR_TAG_STR_" + dynamic_pointer_cast<const CallTagOperation>(OP)->Function);
                break;
            case OperationType::RELEASE:
                #warning TODO release
                data = Null_Const;
        }
    } else {
        connective = (int64_t)form->type;
        std::vector<Constant*> childConsts;
        for (std::shared_ptr<ContractFormula> child : form->Children) {
            childConsts.push_back(createFormulaGlobal(M, child));
        }
        ArrayType* ArrChildren = ArrayType::get(Formula_Type, childConsts.size());
        children = createConstantGlobalUnique(M, ConstantArray::get(ArrChildren, childConsts), "CONTRACT_CHILDREN");
    }
    return ConstantStruct::get(Formula_Type, {children, ConstantInt::get(Int_Type, form->Children.size()), ConstantInt::get(Int_Type, connective), data});
}

GlobalVariable* InstrumentPass::createConstantGlobalUnique(Module& M, Constant* C, std::string name) {
    static uint64_t globals_counter = 0; // For name uniqueness
    return createConstantGlobal(M, C, name + "_" + std::to_string(globals_counter++));
}


GlobalVariable* InstrumentPass::createConstantGlobal(Module& M, Constant* C, std::string name) {
    GlobalVariable* GV = dyn_cast<GlobalVariable>(M.getOrInsertGlobal(name, C->getType()));
    GV->setInitializer(C);
    return GV;
}

void InstrumentPass::createTypes(Module& M) {
    // Basic Types
    Ptr_Type = PointerType::get(M.getContext(), 0);
    Int_Type = IntegerType::get(M.getContext(), 64);
    Null_Const = ConstantPointerNull::getNullValue(Ptr_Type);

    // Operations
    CallOp_Type = StructType::create(M.getContext(), "CallOp_t");
    CallOp_Type->setBody({Ptr_Type, Ptr_Type}); // Function Pointer, char* Function Name

    CallTagOp_Type = StructType::create(M.getContext(), "CallTagOp_t");
    CallTagOp_Type->setBody(Ptr_Type); // char* Tag name

    // Composite Types
    Formula_Type = StructType::create(M.getContext(), "ContractFormula_t");
    Formula_Type->setBody({Ptr_Type, Int_Type, Int_Type, Ptr_Type}); // Children, number of children, connective, expression data ptr

    Contract_Type = StructType::create(M.getContext(), "Contract_t");
    Contract_Type->setBody({Ptr_Type, Ptr_Type, Ptr_Type, Ptr_Type}); // Precondition ptr, Postcondition ptr, contr supplier ptr, supplier name
}
