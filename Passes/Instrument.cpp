#include "Instrument.hpp"
#include "ContractManager.hpp"
#include "ContractPassUtility.hpp"
#include "ContractTree.hpp"
#include "ErrorMessage.h"
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <json/reader.h>
#include <json/value.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Compiler.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Support/WithColor.h>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;
using namespace ContractTree;

static cl::opt<std::string> ClInstrumentType(
    "cover-instrument-type", cl::init("full"),
    cl::desc("Kind of instrumentation to apply. Choices: full, filtered[=<detection json>], funconly"),
    cl::Hidden);

PreservedAnalyses InstrumentPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    DB = &AM.getResult<ContractManagerAnalysis>(M);
    Basic_Types = AM.getResult<BasicTypesAnalysis>(M);

    Function* mainF = M.getFunction("main");
    if (!mainF) return PreservedAnalyses::all(); // No point
    if (M.getFunction("_QQmain")) isC = false; // TODO: Switch to DISourceLanguage check once released

    // Read detJson
    Json::Value detJson;
    if (ClInstrumentType.starts_with("filtered=")) {
        std::ifstream  json_in(ClInstrumentType.substr(9, std::string::npos));
        if (!json_in) {
            WithColor(errs(), HighlightColor::Error) << "Given JSON file could not be opened!\nEnsure the path is correct!\n";
            exit(EXIT_FAILURE);
        }
        Json::Reader reader;
        reader.parse(json_in, detJson);
        if (detJson.get("messages", Json::nullValue) == Json::nullValue) {
            WithColor(errs(), HighlightColor::Error) << "Given JSON file is of invalid format!\n";
            exit(EXIT_FAILURE);
        }
    } else {
        detJson = DB->processedReports;
    }
    // Fill references
    for (Json::Value msg_j : detJson["messages"]) {
        ErrorMessage msg = {msg_j["type"].asString(), msg_j["error_id"].asString(), msg_j["text"].asString()};
        for (Json::Value ref : msg_j["references"]) {
            msg.references.push_back({
                ref["file"].asString(),
                ref["line"].asUInt(),
                ref["column"].asUInt()
            });
        }
        err_msgs.push_back(msg);
    }

    // Contract Types and consts
    createTypes(M);

    // Create Tag globals
    Constant* TagVal;
    TagVal = createTagGlobal(M);

    // Create reference globals
    Constant* ReferencesVal;
    uint64_t num_refs;
    std::tie(ReferencesVal, num_refs) = createReferencesGlobal(M);

    // Create Contract globals
    Constant* ContractsVal;
    uint64_t num_contrs;
    std::tie(ContractsVal, num_contrs) = createContractsGlobal(M);

    // Package database
    GlobalVariable* GlobalDB = dyn_cast<GlobalVariable>(M.getOrInsertGlobal("CONTR_DB", DB_Type));
    Constant* CDB = ConstantStruct::get(DB_Type, {ContractsVal, Basic_Types.getInt(num_contrs),  TagVal, ReferencesVal, Basic_Types.getInt(num_refs)});
    GlobalDB->setInitializer(CDB);

    AttributeList fnAttr;
    fnAttr = fnAttr.addFnAttribute(M.getContext(), Attribute::NoUnwind);
    fnAttr = fnAttr.addFnAttribute(M.getContext(), Attribute::WillReturn);
    fnAttr = fnAttr.addFnAttribute(M.getContext(), Attribute::NoCallback);

    // Create callback function for rel func call
    // Call sig: isRel, Function ptr, ret size, num operands, vararg list of operands. Format: {int64-as-bool isptr, size of param, param} for each param.
    FunctionType* FunctionCBType = FunctionType::get(Basic_Types.Ptr_Type, {Basic_Types.Bool_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type, Basic_Types.Int_Type}, true);
    callbackFuncCallee = M.getOrInsertFunction("PPDCV_FunctionCallback", FunctionCBType, fnAttr);
    Function* callbackFunc = dyn_cast<Function>(callbackFuncCallee.getCallee());
    callbackFunc->setLinkage(GlobalValue::ExternalWeakLinkage);

    // Create callback function for RW
    // Call sig: int64-as-bool isWrite, mem ptr
    FunctionType* FunctionRWType = FunctionType::get(Basic_Types.Void_Type, {Basic_Types.Bool_Type, Basic_Types.Ptr_Type}, false);
    callbackRCallee = M.getOrInsertFunction("PPDCV_MemRCallback", FunctionRWType, fnAttr);
    Function* callbackR = dyn_cast<Function>(callbackRCallee.getCallee());
    callbackR->setLinkage(GlobalValue::ExternalWeakLinkage);
    callbackWCallee = M.getOrInsertFunction("PPDCV_MemWCallback", FunctionRWType, fnAttr);
    Function* callbackW = dyn_cast<Function>(callbackWCallee.getCallee());
    callbackW->setLinkage(GlobalValue::ExternalWeakLinkage);

    // Finally the init routine
    FunctionType* InitCBType = FunctionType::get(Basic_Types.Void_Type, {Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Ptr_Type}, false);
    initFuncCallee = M.getOrInsertFunction("PPDCV_Initialize", InitCBType, fnAttr);
    Function* initFunc = dyn_cast<Function>(initFuncCallee.getCallee());
    initFunc->setLinkage(GlobalValue::ExternalWeakLinkage);

    // Create initialization routine for tool
    Value* Vargc = mainF->getArg(0);
    Value* Vargv = mainF->getArg(1);
    AllocaInst* argcptr = new AllocaInst(Basic_Types.Int_Type, 0, "argc_ptr", mainF->getEntryBlock().getFirstNonPHIOrDbg());
    AllocaInst* argvptr = new AllocaInst(Basic_Types.Ptr_Type, 0, "argv_ptr", argcptr->getIterator());
    CallInst* initFuncCI = CallInst::Create(initFuncCallee, {argcptr, argvptr, GlobalDB});
    initFuncCI->insertAfter(argcptr->getIterator());
    instrument_ignore.insert({argcptr, argvptr});
    instrument_ignore.insert(new StoreInst(Vargc, argcptr, initFuncCI->getIterator()));
    instrument_ignore.insert(new StoreInst(Vargv, argvptr, initFuncCI->getIterator()));

    // Create callbacks
    if (ClInstrumentType != "funconly")
        instrumentRW(M);
    instrumentFunctions(M);

    return PreservedAnalyses::none();
}

Constant* InstrumentPass::createTagGlobal(Module& M) {
    // Create Tags
    std::vector<Constant*> tags;
    std::vector<Constant*> funcs;
    int count = 0;
    for (std::pair<Function*, std::vector<TagUnit>> functags : DB->Tags) {
        for (TagUnit tag : functags.second) {
            Constant* param = ConstantInt::get(Basic_Types.Int_Type, tag.param ? *tag.param : -1);
            Constant* str = ConstantDataArray::getString(M.getContext(), tag.tag);
            GlobalVariable* strGlobal = createConstantGlobal(M, str, "CONTR_TAG_STR_" + tag.tag);
            Constant* TagC = ConstantStruct::get(Tag_Type, {strGlobal,param});
            funcs.push_back(functags.first);
            tags.push_back(TagC);
            count++;
        }
    }

    // Create global const arrays for the tags
    ArrayType* ArrFuncTy = ArrayType::get(Basic_Types.Ptr_Type, count);
    ArrayType* ArrTagTy = ArrayType::get(Tag_Type, count);
    GlobalVariable* ptrFuncs = createConstantGlobal(M, ConstantArray::get(ArrFuncTy, funcs), "CONTR_TAG_ARRAY_PTRS");
    GlobalVariable* ptrTags = createConstantGlobal(M, ConstantArray::get(ArrTagTy, tags), "CONTR_TAG_ARRAY_TAGS");

    // Full tag map structure
    Constant* TagsStruct = ConstantStruct::get(Tags_Type, {ptrFuncs, ptrTags, ConstantInt::get(Basic_Types.Int_Type, count)});
    return TagsStruct;
}

std::pair<Constant*, int64_t> InstrumentPass::createReferencesGlobal(Module &M) {
    std::vector<Constant*> crefs;
    for (ErrorMessage const& msg : err_msgs) {
        GlobalVariable* emsg = createConstantGlobal(M, ConstantDataArray::getString(M.getContext(), msg.type), "CONTR_ERROR_TYPE_" + msg.type);
        for (FileReference const& ref : msg.references) {
            std::string reference_str = ref.file + ":" + std::to_string(ref.line);
            GlobalVariable* fref = createConstantGlobal(M, ConstantDataArray::getString(M.getContext(), reference_str), "CONTR_REFERENCE_" + reference_str);
            crefs.push_back(ConstantStruct::get(Ref_Type, {fref, emsg}));
        }
    }
    ArrayType* ArrRefs = ArrayType::get(Ref_Type, crefs.size());
    GlobalVariable* arrRefsGlobal = createConstantGlobal(M, ConstantArray::get(ArrRefs, crefs), "CONTR_LIST_REFERENCES");
    return {arrRefsGlobal, crefs.size()};
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
    if (forms.empty()) return Basic_Types.Null_Const;
    for (std::shared_ptr<ContractFormula> form : forms) {
        formsConst.push_back(createFormulaGlobal(M, form));
    }
    ArrayType* ArrPreCond = ArrayType::get(Formula_Type, forms.size());
    GlobalVariable* Sublevel = createConstantGlobalUnique(M, ConstantArray::get(ArrPreCond, formsConst), std::string("CONTR_SCOPECONDITIONS"));
    return createConstantGlobalUnique(M, ConstantStruct::get(Formula_Type, { Sublevel, Basic_Types.getInt(forms.size()), Basic_Types.getInt((int64_t)FormulaType::AND), scopeMsgConst, Basic_Types.Null_Const}), "CONTR_SCOPE");
}

Constant* InstrumentPass::createFormulaGlobal(Module& M, std::shared_ptr<ContractFormula> form) {
    Constant* op_const = Basic_Types.Null_Const;
    Constant* children = Basic_Types.Null_Const;
    Constant* msg = Basic_Types.Null_Const;
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
    return ConstantStruct::get(Formula_Type, {children, Basic_Types.getInt(form->Children.size()), Basic_Types.getInt(connective), msg, op_const});
}

Constant* InstrumentPass::createOperationGlobal(Module& M, std::shared_ptr<const Operation> op) {
    Constant* data = Basic_Types.Null_Const;
    std::string name = "UNKNOWN";
    switch (op->type()) {
        case FormulaType::AND:
        case FormulaType::OR:
        case FormulaType::XOR:
            // Should not happen here!
            errs() << "Unexpected connective in createOperationGlobal!\n";
            break;
        case FormulaType::READ:
        case FormulaType::WRITE:
        case FormulaType::RWOP: {
            std::shared_ptr<const RWOperation> rwOP = static_pointer_cast<const RWOperation>(op);
            Constant* isWrite = Basic_Types.getBool(op->type() == FormulaType::WRITE);
            ConstantInt* const_paramacc = Basic_Types.getInt((int)rwOP->contrParamAccess);
            ConstantInt* const_idx = Basic_Types.getInt((int)rwOP->contrP);
            data = ConstantStruct::get(RWOp_Type, {const_idx, const_paramacc, isWrite});
            name = "CONTR_RWOP";
            break;
        }
        case FormulaType::CALL: {
            std::shared_ptr<const CallOperation> cOP = static_pointer_cast<const CallOperation>(op);
            Function* F = M.getFunction(cOP->Function) ? M.getFunction(cOP->Function) : M.getFunction(StringRef(cOP->Function).lower() + "_");
            if (!F) WithColor::warning() << "Specified function \"" << cOP->Function << "\" in calloperation does not exist or unused in module. This may cause issues for instrumentation.\n";
            else mentioned_funcs.push_back(F);
            Constant* funcStr = ConstantDataArray::getString(M.getContext(), cOP->Function);
            std::pair<Constant*,int64_t> paramGlobal = createParamList(M, cOP->Params);
            data = ConstantStruct::get(CallOp_Type, {createConstantGlobal(M, funcStr, "CONTR_FUNC_STR_" + cOP->Function), paramGlobal.first, Basic_Types.getInt(paramGlobal.second), F ? F : Basic_Types.Null_Const});
            name = "CONTR_CALLOP";
            break;
        }
        case FormulaType::CALLTAG: {
            std::shared_ptr<const CallOperation> cOP = static_pointer_cast<const CallOperation>(op);
            data = createConstantGlobal(M, ConstantDataArray::getString(M.getContext(), cOP->Function), "CONTR_TAG_STR_" + cOP->Function);
            std::pair<Constant*,int64_t> paramGlobal = createParamList(M, cOP->Params);
            data = ConstantStruct::get(CallTagOp_Type, {data, paramGlobal.first, Basic_Types.getInt(paramGlobal.second)});
            name = "CONTR_CALLTAGOP";
            break;
        }
        case FormulaType::RELEASE: {
            std::shared_ptr<const ReleaseOperation> rOP = static_pointer_cast<const ReleaseOperation>(op);
            Constant* forbidden_op = createOperationGlobal(M, rOP->Forbidden);
            Constant* forb_type = Basic_Types.getInt((int64_t)rOP->Forbidden->type());
            Constant* release_op = createOperationGlobal(M, rOP->Until);
            Constant* release_type = Basic_Types.getInt((int64_t)rOP->Until->type());
            data = ConstantStruct::get(ReleaseOp_Type, {release_op, release_type, forbidden_op, forb_type});
            name = "CONTR_RELEASE";
            break;
        }
        case FormulaType::PARAM: {
            std::shared_ptr<const ParamOperation> pOP = static_pointer_cast<const ParamOperation>(op);
            std::vector<Constant*> reqCs;
            bool hasIntCmp = false;
            for (ParamRequirement const& req : pOP->reqs) {
                Constant* var = Basic_Types.Null_Const;
                try {
                    int ivalue = std::stoi(req.value);
                    var = Basic_Types.getInt64(ivalue);
                    var = ConstantExpr::getIntToPtr(var, Basic_Types.Ptr_Type);
                    reqCs.push_back(ConstantStruct::get(ParamReq_Type, {Basic_Types.getInt(req.comp), var, Basic_Types.getBool(req.isArg), Basic_Types.getBool(false)}));
                    hasIntCmp = req.isArg ? hasIntCmp : true;
                } catch(std::exception& e) {
                    if (!DB->ContractVariableData.contains(req.value)) {
                        errs() << "Undefined non-constint contract value identifier \"" << req.value << "\"!\n";
                        errs() << "Param Requirement will not be instrumented!\n";
                        continue;
                    }
                    for (Value* V : DB->ContractVariableData[req.value]) {
                        if (isa<Constant>(V)) var = (Constant*)V;
                        if (isa<ConstantInt>(var)) var = ConstantExpr::getIntToPtr(var, Basic_Types.Ptr_Type);
                        if (!isa<Constant>(var)) {
                            errs() << "Weird param error in instr pass\n";
                        }
                        reqCs.push_back(ConstantStruct::get(ParamReq_Type, {Basic_Types.getInt(req.comp), var, Basic_Types.getBool(req.isArg), Basic_Types.getBool(!isC && (var->getName().starts_with("_QQ")))}));
                        hasIntCmp = ContractPassUtility::fortCheckAndGetGlbInt(var) ? true : hasIntCmp;
                    }
                }
            }
            Constant* reqsC = ConstantArray::get(ArrayType::get(ParamReq_Type, reqCs.size()), reqCs);
            reqsC = createConstantGlobalUnique(M, reqsC, "CONTR_PARAM_REQS");
            data = ConstantStruct::get(ParamOp_Type, {Basic_Types.getInt(pOP->idx), reqsC, Basic_Types.getInt(reqCs.size()), Basic_Types.getBool(!isC && hasIntCmp)});
            name = "CONTR_PARAMOP";
            break;
        }
        case FormulaType::ALLOC: {
            static std::vector<Constant*> allocators;
            static std::vector<Constant*> deallocators;
            static Constant* allocs_C = Basic_Types.Null_Const;
            static Constant* deallocs_C = Basic_Types.Null_Const;
            std::shared_ptr<const AllocOperation> allocOp = std::static_pointer_cast<const AllocOperation>(op);
            if (allocators.empty() && deallocators.empty()) {
                for (ContractManagerAnalysis::LinearizedContract const& C : DB->LinearizedContracts) {
                    for (const std::shared_ptr<ContractExpression> Expr : C.Post) {
                        switch (Expr->OP->type()) {
                            case FormulaType::ALLOC:
                            case FormulaType::FREE: {
                                std::shared_ptr<const RWOperation> rwOp = std::make_shared<const RWOperation>(*std::static_pointer_cast<const RWOperation>(Expr->OP));
                                Constant* rwOp_C = createOperationGlobal(M, rwOp);
                                Constant* memop_C = ConstantStruct::get(MemOpFunc_Type, {C.F, rwOp_C, Expr->OP->type() == FormulaType::ALLOC ? createMathExprGlobal(M, std::static_pointer_cast<AllocOperation const>(Expr->OP)->size) : Basic_Types.Null_Const});
                                if (Expr->OP->type() == FormulaType::ALLOC) allocators.push_back(memop_C);
                                else if (Expr->OP->type() == FormulaType::FREE) deallocators.push_back(memop_C);
                                else llvm_unreachable("Unexpected type when constructing alloc/free inst!");
                                break;
                            }
                            default: continue;
                        }
                    }
                }
                static ArrayType* allocators_Type = ArrayType::get(MemOpFunc_Type, allocators.size());
                allocs_C = ConstantArray::get(allocators_Type, allocators);
                allocs_C = createConstantGlobal(M, allocs_C, "CONTR_ALLOCATOR_LIST");
                static ArrayType* deallocators_Type = ArrayType::get(MemOpFunc_Type, deallocators.size());
                deallocs_C = ConstantArray::get(deallocators_Type, deallocators);
                deallocs_C = createConstantGlobal(M, deallocs_C, "CONTR_DEALLOCATOR_LIST");
            }
            data = ConstantStruct::get(AllocOp_Type, {Basic_Types.getInt(allocOp->contrP), Basic_Types.getInt((int32_t)allocOp->contrParamAccess),
                                                           allocs_C, Basic_Types.getInt(allocators.size()),
                                                           deallocs_C, Basic_Types.getInt(deallocators.size())});
            name = "CONTR_ALLOCOP";
            break;
        }
        case FormulaType::FREE: break;
    }
    return createConstantGlobalUnique(M, data, name);
}

Constant* InstrumentPass::createMathExprGlobal(Module& M, std::shared_ptr<MathExpr> expr) {
    Constant* val = Basic_Types.getInt(expr->value);
    Constant* isArg = Basic_Types.getBool(expr->isArg);
    Constant* type = Basic_Types.getInt((int32_t)expr->type);
    Constant* other = Basic_Types.Null_Const;
    if (expr->type != MathType::UNARY_VALUE) {
        other = createMathExprGlobal(M, expr->other);
    }
    Constant* result = ConstantStruct::get(MathExpr_Type, {val, isArg, type, other});
    return createConstantGlobalUnique(M, result, "CONT_MATHEXPR");
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
    // Operations
    Param_Type = StructType::create(M.getContext(), "CallParam_t");
    Param_Type->setBody({Basic_Types.Int_Type, Basic_Types.Bool_Type, Basic_Types.Int_Type, Basic_Types.Int_Type}); // call param, bool param is tag ref, contr param, acc type

    CallOp_Type = StructType::create(M.getContext(), "CallOp_t");
    CallOp_Type->setBody({Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type, Basic_Types.Ptr_Type}); // char* Function Name, list of params, num of params, Function Pointer

    CallTagOp_Type = StructType::create(M.getContext(), "CallTagOp_t");
    CallTagOp_Type->setBody({Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type}); // char* Tag name, list of params, num of params

    ReleaseOp_Type = StructType::create(M.getContext(), "ReleaseOp_t");
    ReleaseOp_Type->setBody({Basic_Types.Ptr_Type, Basic_Types.Int_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type}); // void* release op, relop type, void* forbidden op, forbop type

    RWOp_Type = StructType::create(M.getContext(), "RWOp_t");
    RWOp_Type->setBody({Basic_Types.Int_Type, Basic_Types.Int_Type, Basic_Types.Bool_Type}); // idx, paramaccess, isWrite

    ParamOp_Type = StructType::create(M.getContext(), "ParamOp_t");
    ParamOp_Type->setBody({Basic_Types.Int_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type, Basic_Types.Bool_Type}); // idx, list of reqs, num reqs, need deref

    AllocOp_Type = StructType::create(M.getContext(), "AllocOp_t");
    AllocOp_Type->setBody({Basic_Types.Int_Type, Basic_Types.Int_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type}); // idx, accType, list of allocators, num allocs, list of deallocs, num deallocs

    // Composite Types
    Tag_Type = StructType::create(M.getContext(), "Tag_t");
    Tag_Type->setBody({Basic_Types.Ptr_Type, Basic_Types.Int_Type}); // tag str, param num

    Formula_Type = StructType::create(M.getContext(), "ContractFormula_t");
    Formula_Type->setBody({Basic_Types.Ptr_Type, Basic_Types.Int_Type, Basic_Types.Int_Type, Basic_Types.Ptr_Type, Basic_Types.Ptr_Type}); // Children, number of children, connective, message char*, expression data ptr

    Contract_Type = StructType::create(M.getContext(), "Contract_t");
    Contract_Type->setBody({Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Ptr_Type}); // Precondition ptr, Postcondition ptr, contr supplier ptr, supplier name

    Tags_Type = StructType::create(M.getContext(), "TagsMap_t");
    Tags_Type->setBody({Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type}); // Funcptr list, Tag + param struct list, num elems

    MathExpr_Type = StructType::create(M.getContext(), "MathExpr_t");
    MathExpr_Type->setBody({Basic_Types.Int_Type, Basic_Types.Bool_Type, Basic_Types.Int_Type, Basic_Types.Ptr_Type}); // int val, bool isarg, math type, other math

    Ref_Type = StructType::create(M.getContext(), "Reference_t");
    Ref_Type->setBody({Basic_Types.Ptr_Type, Basic_Types.Ptr_Type}); // char* file ref, char* type

    ParamReq_Type = StructType::create(M.getContext(), "ParamReq_t");
    ParamReq_Type->setBody({Basic_Types.Int_Type, Basic_Types.Ptr_Type, Basic_Types.Bool_Type, Basic_Types.Bool_Type}); // Comparator, Value, isArg, need_deref

    MemOpFunc_Type = StructType::create(M.getContext(), "MemOpFunc_t");
    MemOpFunc_Type->setBody({Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Ptr_Type}); // Func, rwOp, size mathexpr

    DB_Type = StructType::create(M.getContext(), "ContractDB_t");
    DB_Type->setBody({Basic_Types.Ptr_Type, Basic_Types.Int_Type, Tags_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type}); // contract list, num elems, tag container, reference list, num refs
}

void InstrumentPass::instrumentFunctions(Module &M) {
    // All functions with attached contracts
    for (ContractManagerAnalysis::Contract C : DB->Contracts) {
        insertFunctionInstrCallback(C.F);
    }

    // All functions referenced in tags
    for (std::pair<Function*, std::vector<TagUnit>> tag : DB->Tags) {
        insertFunctionInstrCallback(tag.first);
    }

    // All functions referenced by name
    for (Function* F : mentioned_funcs) {
        insertFunctionInstrCallback(F);
    }
}

void InstrumentPass::instrumentRW(Module &M) {
    for (Function& F : M) {
        for (BasicBlock& BB : F) {
            for (Instruction& I : BB) {
                if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
                    if (instrument_ignore.contains(&I)) continue;
                    Value* V = getLoadStorePointerOperand(&I);
                    // Fortran: Check if reading array metadata, and skip callback if so
                    if (GEPOperator const* GEPOp = dyn_cast<GEPOperator>(V)) {
                        if (GlobalVariable const* GV = dyn_cast<GlobalVariable>(GEPOp->getPointerOperand())) {
                            SmallVector<DIGlobalVariableExpression*> dbg_arr;
                            GV->getDebugInfo(dbg_arr);
                            if (!isC && !dbg_arr.empty() && dbg_arr[0]->getVariable()->getType()->getTag() == dwarf::DW_TAG_array_type) {
                                continue;
                            }
                        }
                    }
                    // Filter out new instr from sroa
                    if (!isC && isa<StoreInst>(&I) && dyn_cast<StoreInst>(&I)->getPointerOperand()->getName().starts_with(".fca.")) {
                        continue;
                    }

                    insertCBIfNeeded(isa<LoadInst>(I) ? callbackRCallee : callbackWCallee, {V}, &I);
                }
            }
        }
    }
}

std::pair<Constant*,int64_t> InstrumentPass::createParamList(Module& M, std::vector<CallParam> params) {
    if (params.empty()) return { Basic_Types.Null_Const, 0 };
    std::vector<Constant*> paramConsts;
    for (CallParam param : params) {
        Constant* pConst = ConstantStruct::get(Param_Type, {Basic_Types.getInt(param.callP), Basic_Types.getBool(param.callPisTagVar), Basic_Types.getInt(param.contrP), Basic_Types.getInt((int32_t)param.contrParamAccess)});
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
        int stringnum = 0;
        std::vector<Value*> params;
        params.push_back(callsite->getCalledOperand()); // First param is funcptr

        // Get return value size
        if (!callsite->getType()->isSized()) {
            params.push_back(Basic_Types.getInt(0));
        } else {
            int ret_size = callsite->getDataLayout().getTypeStoreSizeInBits(callsite->getType());
            if (callsite->getType()->isFloatingPointTy()) ret_size |= 2 << 16; // Set bit 2 > float tag
            params.push_back(Basic_Types.getInt(ret_size));
        }

        params.push_back(Basic_Types.getInt(callsite->arg_size()));
        for (Use const& U : callsite->args()) {
            Value* actual_param = U;
            int const cur_argno = callsite->getArgOperandNo(&U);

            // Store size of data type
            if (isC) {
                int size_act = callsite->getDataLayout().getTypeStoreSizeInBits(U->getType());
                params.push_back(Basic_Types.getInt((size_act << 8) | (size_act & 0xFF)));
                // Store actual parameter, making sure to cast if necessary
                if (!U->getType()->isPointerTy()) {
                    if (U->getType()->isFloatingPointTy()) {
                        actual_param = CastInst::Create(Instruction::CastOps::BitCast, actual_param, Basic_Types.Int_Type, "", callsite->getIterator());
                    }
                    // Now, actual pointer cast
                    actual_param = CastInst::Create(Instruction::CastOps::IntToPtr, actual_param, Basic_Types.Ptr_Type, "", callsite->getIterator());
                }
            } else {
                if (Function const* F = dyn_cast<Function>(callsite->getCalledOperand())) {
                    DISubprogram const* Dbg = F->getSubprogram();
                    int size_call = 0;
                    int size_act = 0;
                    // Vararg intrinsics have to be handled specially due to missing debug info on vararg
                    if (F->getName() == "CoVer_FPointerAllocate") {
                            // Prefix - 0, 1 ptr and size, 2 num_dims
                            switch (cur_argno) {
                                case 0:
                                case 1: size_act = size_call = 64; break;
                                case 2: size_act = size_call = 32;
                            }
                            if (!size_act) {
                                // End - _FortranAPointerAllocate params
                                if (cur_argno == callsite->arg_size() - 4) size_act = size_call = 32;
                                if (cur_argno == callsite->arg_size() - 3) size_act = size_call = 64;
                                if (cur_argno == callsite->arg_size() - 2) size_act = size_call = 64;
                                if (cur_argno == callsite->arg_size() - 1) size_act = size_call = 32;
                            }
                            if (!size_act) {
                                // Middle - Descriptors for dims
                                int tmp = cur_argno - 3;
                                if (tmp % 3 == 0) size_act = size_call = 32;
                                else size_act = size_call = 64;
                            }
                    } else {
                        if (cur_argno >= callsite->arg_size() - stringnum) {
                            size_act = size_call = callsite->getParent()->getDataLayout().getTypeAllocSize(actual_param->getType());
                        } else {
                            if (checkIsStrParam(U)) stringnum++;
                            // All parameters are sent as pointers. Need to check exact size using dbg info
                            DIType const* param_type = Dbg->getType()->getTypeArray()[cur_argno + 1]; // Offset by one, first is ret val
                            size_act = param_type->getSizeInBits() == 0 || isa<GlobalValue>(actual_param) ? 64 : param_type->getSizeInBits();
                            size_call = callsite->getDataLayout().getTypeStoreSizeInBits(U->getType());
                            // On Fortran, deref if param is an allocate/ptr buffer.
                            // Indicate with magic bit
                            if (param_type->getTag() == dwarf::DW_TAG_array_type) {
                                size_call = size_call | 1 << 8;
                            }
                        }
                    }
                    params.push_back(Basic_Types.getInt((size_call << 8) | (size_act & 0xFF)));
                }
            }
            params.push_back(actual_param);
        }
        insertCBIfNeeded(callbackFuncCallee, params, callsite);
    }
    already_instrumented.insert(F);
}

void InstrumentPass::insertCBIfNeeded(FunctionCallee FC, std::vector<Value *> params, Instruction* I) {
    if (!isRelevant(I) && (isa<LoadInst>(I) || isa<StoreInst>(I)) && ClInstrumentType.starts_with("filtered")) return;
    params.insert(params.begin(), Basic_Types.getBool(isRelevant(I)));
    CallInst* callbackCI = CallInst::Create(FC, params);
    callbackCI->setDebugLoc(I->getDebugLoc());
    if (CallBase* CB = dyn_cast<CallBase>(I)) {
        Type* OrigRT = CB->getCalledFunction()->getReturnType();
        if (!OrigRT->isPointerTy() && !OrigRT->isVoidTy()) {
            callbackCI->insertBefore(I->getIterator());
            if (OrigRT->isIntegerTy()) {
                CastInst* CI = CastInst::Create(Instruction::PtrToInt, callbackCI, OrigRT);
                ReplaceInstWithInst(I, CI);
            } else if (OrigRT->isFloatingPointTy()) {
                CastInst* CI = CastInst::Create(Instruction::PtrToInt, callbackCI, Basic_Types.Int64_Type, "", I->getIterator());
                CI = CastInst::Create(Instruction::BitCast, CI, OrigRT);
                ReplaceInstWithInst(I, CI);
            }
        } else {
            ReplaceInstWithInst(I, callbackCI);
        }
    }
    else callbackCI->insertBefore(I->getIterator());
}

bool InstrumentPass::isRelevant(Instruction const* I) const {
    FileReference f = ContractPassUtility::getFileReference(I);
    for (ErrorMessage const& msg : err_msgs) {
        for (FileReference const& ref : msg.references) if (ref == f) return true;
    }
    return false;
}

bool InstrumentPass::checkIsStrParam(Value const* V) {
    // We want to check if I is a string param. If so, instrumentation should omit the string size arg
    // Lowered FIR does not make this easy.
    // If its a str var, its just some global, then the str size appended as another (fake) param
    // Otherwise, its "fun" with extract and insert value insts.
    // Need to use a heuristic approach to check
    if (ExtractValueInst const* EV = dyn_cast<ExtractValueInst>(V)) {
        // String operand extract...
        // Also, check if its operand 0 (1 would be str len)
        if (EV->getNumIndices() != 1 || EV->getIndices()[0] != 0) return false;
        if (InsertValueInst const* IV = dyn_cast<InsertValueInst>(EV->getAggregateOperand())) {
            // Insertion of size...
            if (InsertValueInst const* IV2 = dyn_cast<InsertValueInst>(IV->getAggregateOperand())) {
                // Insertion of str... seems legit
                // Final Check: Struct of the type we expect
                StructType const* T = dyn_cast<StructType>(IV2->getType());
                return T && T->getElementType(0)->isPointerTy() && T->getElementType(1)->isIntegerTy(64);
            }
        }
    }

    // Now, check if its a global string
    if (GlobalVariable const* GV = dyn_cast<GlobalVariable>(V)) {
        Constant const* Init = GV->getInitializer();
        return Init && !Init->isZeroValue() && isa<ArrayType>(Init->getType()) && dyn_cast<ArrayType>(Init->getType())->getElementType() == IntegerType::get(V->getContext(), 8);
    }
    return false;
}
