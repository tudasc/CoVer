#include "Instrument.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
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
    Constant* TagVal;
    TagVal = createTagGlobal(M);

    // Create Contract globals
    Constant* ContractsVal;
    uint64_t num_contrs;
    std::tie(ContractsVal, num_contrs) = createContractsGlobal(M);

    // Package database
    GlobalVariable* GlobalDB = dyn_cast<GlobalVariable>(M.getOrInsertGlobal("CONTR_DB", DB_Type));
    Constant* CDB = ConstantStruct::get(DB_Type, {ContractsVal, ConstantInt::get(Int_Type, num_contrs),  TagVal});
    GlobalDB->setInitializer(CDB);

    // Create initialization routine for tool
    FunctionCallee initFuncCallee = M.getOrInsertFunction("PPDCV_Initialize", Void_Type, Ptr_Type);
    Function* initFunc = dyn_cast<Function>(initFuncCallee.getCallee());
    initFunc->setLinkage(GlobalValue::ExternalWeakLinkage);
    CallInst* initFuncCI = CallInst::Create(initFuncCallee, GlobalDB);
    initFuncCI->insertBefore(mainF->getEntryBlock().getFirstNonPHIOrDbg());

    // Create callback function for rel func call
    // Call sig: Function ptr, num operands, vararg list of operands. Format: {int64-as-bool isptr, size of param, param} for each param.
    FunctionType* FunctionCBType = FunctionType::get(Void_Type, {Ptr_Type, Int_Type}, true);
    callbackFuncCallee = M.getOrInsertFunction("PPDCV_FunctionCallback", FunctionCBType);
    Function* callbackFunc = dyn_cast<Function>(callbackFuncCallee.getCallee());
    callbackFunc->setLinkage(GlobalValue::ExternalWeakLinkage);

    // Create callback function for RW
    // Call sig: int64-as-bool isWrite, mem ptr
    callbackRWCallee = M.getOrInsertFunction("PPDCV_MemCallback", Void_Type, Int_Type, Ptr_Type);
    Function* callbackRW = dyn_cast<Function>(callbackRWCallee.getCallee());
    callbackRW->setLinkage(GlobalValue::ExternalWeakLinkage);

    // Create callbacks
    instrumentFunctions(M);
    instrumentRW(M);

    return PreservedAnalyses::all();
}

Constant* InstrumentPass::createTagGlobal(Module& M) {
    // Create Tags
    std::vector<Constant*> tags;
    std::vector<Constant*> funcs;
    int count = 0;
    for (std::pair<Function*, std::vector<TagUnit>> functags : DB->Tags) {
        for (TagUnit tag : functags.second) {
            Constant* param = ConstantInt::get(Int_Type, tag.param ? *tag.param : -1);
            Constant* str = ConstantDataArray::getString(M.getContext(), tag.tag);
            GlobalVariable* strGlobal = createConstantGlobal(M, str, "CONTR_TAG_STR_" + tag.tag);
            Constant* TagC = ConstantStruct::get(Tag_Type, {strGlobal,param});
            funcs.push_back(functags.first);
            tags.push_back(TagC);
            count++;
        }
    }

    // Create global const arrays for the tags
    ArrayType* ArrFuncTy = ArrayType::get(Ptr_Type, count);
    ArrayType* ArrTagTy = ArrayType::get(Tag_Type, count);
    GlobalVariable* ptrFuncs = createConstantGlobal(M, ConstantArray::get(ArrFuncTy, funcs), "CONTR_TAG_ARRAY_PTRS");
    GlobalVariable* ptrTags = createConstantGlobal(M, ConstantArray::get(ArrTagTy, tags), "CONTR_TAG_ARRAY_TAGS");

    // Full tag map structure
    Constant* TagsStruct = ConstantStruct::get(Tags_Type, {ptrFuncs, ptrTags, ConstantInt::get(Int_Type, count)});
    return TagsStruct;
}

std::pair<Constant*, int64_t> InstrumentPass::createContractsGlobal(Module& M) {
    std::vector<Constant*> contractConsts;
    for (ContractManagerAnalysis::Contract C : DB->Contracts) {
        if (C.Data.Pre.empty() && C.Data.Post.empty()) continue;
        Constant* PrecondConst = createScopeGlobal(M, C.Data.Pre);
        Constant* PostcondConst = createScopeGlobal(M, C.Data.Post);
        GlobalVariable* strGlobal = createConstantGlobal(M, ConstantDataArray::getString(M.getContext(), C.F->getName()), "CONTR_FUNC_STR_" + C.F->getName().str());
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
    static Constant* scopeMsgConst = createConstantGlobal(M, ConstantDataArray::getString(M.getContext(), "Full Scope"), "CONTR_SCOPE_STR_");
    if (forms.empty()) return Null_Const;
    for (std::shared_ptr<ContractFormula> form : forms) {
        formsConst.push_back(createFormulaGlobal(M, form));
    }
    ArrayType* ArrPreCond = ArrayType::get(Formula_Type, forms.size());
    GlobalVariable* Sublevel = createConstantGlobalUnique(M, ConstantArray::get(ArrPreCond, formsConst), std::string("CONTR_SCOPECONDITIONS"));
    return createConstantGlobalUnique(M, ConstantStruct::get(Formula_Type, { Sublevel, ConstantInt::get(Int_Type, forms.size()), ConstantInt::get(Int_Type, (int64_t)FormulaType::AND), scopeMsgConst, Null_Const}), "CONTR_SCOPE");
}

Constant* InstrumentPass::createFormulaGlobal(Module& M, std::shared_ptr<ContractFormula> form) {
    Constant* op_const = Null_Const;
    Constant* children = Null_Const;
    Constant* msg = Null_Const;
    std::string descriptor = form->Message ? form->Message->text : form->ExprStr;
    msg = createConstantGlobal(M, ConstantDataArray::getString(M.getContext(), descriptor), "CONTR_MSG_" + descriptor);
    int64_t connective;
    if (form->Children.empty()) {
        // Expression
        std::shared_ptr<const Operation> OP = static_pointer_cast<ContractExpression>(form)->OP;
        op_const = createOperationGlobal(M, OP);
        connective = (int64_t)OP->type();
    } else {
        connective = (int64_t)form->type;
        std::vector<Constant*> childConsts;
        for (std::shared_ptr<ContractFormula> child : form->Children) {
            childConsts.push_back(createFormulaGlobal(M, child));
        }
        ArrayType* ArrChildren = ArrayType::get(Formula_Type, childConsts.size());
        children = createConstantGlobalUnique(M, ConstantArray::get(ArrChildren, childConsts), "CONTRACT_CHILDREN");
    }
    return ConstantStruct::get(Formula_Type, {children, ConstantInt::get(Int_Type, form->Children.size()), ConstantInt::get(Int_Type, connective), msg, op_const});
}

Constant* InstrumentPass::createOperationGlobal(Module& M, std::shared_ptr<const Operation> op) {
    Constant* data = Null_Const;
    std::string name;
    switch (op->type()) {
        case OperationType::READ:
        case OperationType::WRITE: {
            std::shared_ptr<const RWOperation> rwOP = static_pointer_cast<const RWOperation>(op);
            ConstantInt* isWrite = ConstantInt::get(Int_Type, op->type() == OperationType::WRITE);
            ConstantInt* const_paramacc = ConstantInt::get(Int_Type, (int)rwOP->contrParamAccess);
            ConstantInt* const_idx = ConstantInt::get(Int_Type, (int)rwOP->contrP);
            data = ConstantStruct::get(RWOp_Type, {const_idx, const_paramacc, isWrite});
            name = "CONTR_RWOP";
            break;
        }
        case OperationType::CALL: {
            std::shared_ptr<const CallOperation> cOP = static_pointer_cast<const CallOperation>(op);
            Function* F = M.getFunction(cOP->Function);
            if (!F) errs() << "Warning: Specified function \"" << cOP->Function << "\" in calloperation does not exist or unused in module\nThis may cause issues for instrumentation.\n";
            Constant* funcStr = ConstantDataArray::getString(M.getContext(), cOP->Function);
            std::pair<Constant*,int64_t> paramGlobal = createParamList(M, cOP->Params);
            data = ConstantStruct::get(CallOp_Type, {createConstantGlobal(M, funcStr, "CONTR_FUNC_STR_" + cOP->Function), paramGlobal.first, ConstantInt::get(Int_Type, paramGlobal.second), F ? F : Null_Const});
            name = "CONTR_CALLOP";
            break;
        }
        case OperationType::CALLTAG: {
            std::shared_ptr<const CallOperation> cOP = static_pointer_cast<const CallOperation>(op);
            data = createConstantGlobal(M, ConstantDataArray::getString(M.getContext(), cOP->Function), "CONTR_TAG_STR_" + cOP->Function);
            std::pair<Constant*,int64_t> paramGlobal = createParamList(M, cOP->Params);
            data = ConstantStruct::get(CallTagOp_Type, {data, paramGlobal.first, ConstantInt::get(Int_Type, paramGlobal.second)});
            name = "CONTR_CALLTAGOP";
            break;
        }
        case OperationType::RELEASE:
            std::shared_ptr<const ReleaseOperation> rOP = static_pointer_cast<const ReleaseOperation>(op);
            Constant* forbidden_op = createOperationGlobal(M, rOP->Forbidden);
            Constant* forb_type = ConstantInt::get(Int_Type, (int64_t)rOP->Forbidden->type());
            Constant* release_op = createOperationGlobal(M, rOP->Until);
            Constant* release_type = ConstantInt::get(Int_Type, (int64_t)rOP->Until->type());
            data = ConstantStruct::get(ReleaseOp_Type, {release_op, release_type, forbidden_op, forb_type});
            name = "CONTR_RELEASE";
            break;
    }
    return createConstantGlobalUnique(M, data, name);
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
    Void_Type = Type::getVoidTy(M.getContext());

    // Operations
    Param_Type = StructType::create(M.getContext(), "CallParam_t");
    Param_Type->setBody({Int_Type, Int_Type, Int_Type, Int_Type}); // call param, bool param is tag ref, contr param, acc type

    CallOp_Type = StructType::create(M.getContext(), "CallOp_t");
    CallOp_Type->setBody({Ptr_Type, Ptr_Type, Int_Type, Ptr_Type}); // char* Function Name, list of params, num of params, Function Pointer

    CallTagOp_Type = StructType::create(M.getContext(), "CallTagOp_t");
    CallTagOp_Type->setBody({Ptr_Type, Ptr_Type, Int_Type}); // char* Tag name, list of params, num of params

    ReleaseOp_Type = StructType::create(M.getContext(), "ReleaseOp_t");
    ReleaseOp_Type->setBody({Ptr_Type, Int_Type, Ptr_Type, Int_Type}); // void* release op, relop type, void* forbidden op, forbop type

    RWOp_Type = StructType::create(M.getContext(), "RWOp_t");
    RWOp_Type->setBody({Int_Type, Int_Type, Int_Type}); // idx, paramaccess, isWrite

    // Composite Types
    Tag_Type = StructType::create(M.getContext(), "Tag_t");
    Tag_Type->setBody({Ptr_Type, Int_Type});

    Formula_Type = StructType::create(M.getContext(), "ContractFormula_t");
    Formula_Type->setBody({Ptr_Type, Int_Type, Int_Type, Ptr_Type, Ptr_Type}); // Children, number of children, connective, message char*, expression data ptr

    Contract_Type = StructType::create(M.getContext(), "Contract_t");
    Contract_Type->setBody({Ptr_Type, Ptr_Type, Ptr_Type, Ptr_Type}); // Precondition ptr, Postcondition ptr, contr supplier ptr, supplier name

    Tags_Type = StructType::create(M.getContext(), "TagsMap_t");
    Tags_Type->setBody({Ptr_Type, Ptr_Type, Int_Type}); // Funcptr list, Tag + param struct list, num elems

    DB_Type = StructType::create(M.getContext(), "ContractDB_t");
    DB_Type->setBody( {Ptr_Type, Int_Type, Tags_Type} ); // contract list, num elems, tag container
}

void InstrumentPass::instrumentFunctions(Module &M) {
    for (ContractManagerAnalysis::Contract C : DB->Contracts) {
        // All functions with attached contracts
        insertFunctionInstrCallback(C.F);
    }

    // All functions referenced by name
    for (ContractManagerAnalysis::LinearizedContract C : DB->LinearizedContracts) {
        for (std::shared_ptr<ContractExpression> const& Expr : C.Pre) {
            if (Expr->OP->type() == OperationType::CALL) {
                std::shared_ptr<const CallOperation> cOP = std::static_pointer_cast<const CallOperation>(Expr->OP);
                if (M.getFunction(cOP->Function)) insertFunctionInstrCallback(M.getFunction(cOP->Function));
            }
        }
        for (std::shared_ptr<ContractExpression> const& Expr : C.Pre) {
            if (Expr->OP->type() == OperationType::CALL) {
                std::shared_ptr<const CallOperation> cOP = std::static_pointer_cast<const CallOperation>(Expr->OP);
                if (M.getFunction(cOP->Function)) insertFunctionInstrCallback(M.getFunction(cOP->Function));
            }
        }
    }

    // All functions referenced in tags
    for (std::pair<Function*, std::vector<TagUnit>> tag : DB->Tags) {
        insertFunctionInstrCallback(tag.first);
    }
    #warning TODO all functions referenced by name only
}

void InstrumentPass::instrumentRW(Module &M) {
    for (Function& F : M) {
        for (BasicBlock& BB : F) {
            for (Instruction& I : BB) {
                if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
                    Value* V = getLoadStorePointerOperand(&I);
                    CallInst* callbackCI = CallInst::Create(callbackRWCallee, { ConstantInt::get(Int_Type, isa<StoreInst>(I) ? 1 : 0), V});
                    callbackCI->setDebugLoc(I.getDebugLoc());
                    callbackCI->insertBefore(I.getIterator());
                }
            }
        }
    }
}

std::pair<Constant*,int64_t> InstrumentPass::createParamList(Module& M, std::vector<CallParam> params) {
    if (params.empty()) return { Null_Const, 0 };
    std::vector<Constant*> paramConsts;
    for (CallParam param : params) {
        Constant* pConst = ConstantStruct::get(Param_Type, {ConstantInt::get(Int_Type, param.callP), ConstantInt::get(Int_Type, param.callPisTagVar), ConstantInt::get(Int_Type, param.contrP), ConstantInt::get(Int_Type, (int64_t)param.contrParamAccess)});
        paramConsts.push_back(pConst);
    }
    ArrayType* paramArr_Type = ArrayType::get(Param_Type, paramConsts.size());
    Constant* res = ConstantArray::get(paramArr_Type, paramConsts);
    return {createConstantGlobalUnique(M, res, "CONTR_PARAMLIST"), paramConsts.size()};
}

void InstrumentPass::insertFunctionInstrCallback(Function* F) {
    if (already_instrumented.contains(F)) return;
    std::vector<CallBase*> callsites;
    for (User* U : F->users()) {
        if (CallBase* CB = dyn_cast<CallBase>(U)) {
            callsites.push_back(CB);
        }
    }
    for (CallBase* callsite : callsites) {
        std::vector<Value*> params;
        params.push_back(callsite->getCalledOperand()); // First param is funcptr
        params.push_back(ConstantInt::get(Int_Type, callsite->arg_size()));
        for (Use& U : callsite->args()) {
            params.push_back(U->getType()->isPointerTy() ? ConstantInt::get(Int_Type, 1) : ConstantInt::get(Int_Type, 0));
            params.push_back(ConstantInt::get(Int_Type, U->getType()->getPrimitiveSizeInBits()));
            params.push_back(U);
        }
        CallInst* callbackCI = CallInst::Create(callbackFuncCallee, params);
        callbackCI->setDebugLoc(callsite->getDebugLoc());
        callbackCI->insertBefore(callsite->getIterator());
    }
    already_instrumented.insert(F);
}
