#include "ContractVerifierParam.hpp"
#include "BasicTypes.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "ContractPassUtility.hpp"

#include <format>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/MemoryDependenceAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Analysis/InlineCost.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>
#include <vector>
#include <memory>
#include <string>

using namespace llvm;
using namespace ContractTree;

PreservedAnalyses ContractVerifierParamPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    ContractManagerAnalysis::ContractDatabase DB = AM.getResult<ContractManagerAnalysis>(M);
    MAM = &AM;
    Basic_Types = MAM->getResult<BasicTypesAnalysis>(M);

    for (ContractManagerAnalysis::LinearizedContract const& C : DB.LinearizedContracts) {
        for (const std::shared_ptr<ContractExpression> Expr : C.Pre) {
            if (*Expr->Status != Fulfillment::UNKNOWN) continue;
            // Contract has a precondition
            std::string err;
            if (Expr->OP->type() != FormulaType::PARAM) continue;
            const ParamOperation* ParamOp = dynamic_cast<const ParamOperation*>(Expr->OP.get());
            C.DebugInfo->push_back("[ContractVerifierParam] Attempting to verify expression: " + Expr->ExprStr);
            Fulfillment resf = Fulfillment::FULFILLED;

            // Perform the check on each callsite
            for (User* U : C.F->users()) {
                if (CallBase* CB = dyn_cast<CallBase>(U)) {
                    for (ParamRequirement const& req : ParamOp->reqs) {
                        // Figure out value(s) to check against
                        std::set<Value*> vars;
                        try {
                            // First, check if constant value provided
                            int ivalue = std::stoi(req.value);
                            if (req.isArg) vars = {CB->getArgOperand(ivalue)};
                            else vars = {ConstantInt::get(Type::getInt64Ty(M.getContext()), ivalue)};
                        } catch(std::exception& e) {
                            // Otherwise, check against value database
                            if (!DB.ContractVariableData.contains(req.value)) {
                                errs() << "Undefined non-constint contract value identifier \"" << req.value << "\"!\n";
                                errs() << "Requirement will not be analysed!\n";
                                continue;
                            }
                            vars = DB.ContractVariableData[req.value];
                        }

                        // Perform check
                        std::string errInfo = "";
                        Fulfillment f = checkParamReq(vars, CB, ParamOp->idx, req.comp, errInfo);
                        if (f == Fulfillment::BROKEN) {
                            resf = Fulfillment::BROKEN;
                                Expr->ErrorInfo->push_back({
                                .error_id = "Param",
                                .text = std::format("{:s} Parameter Index: {:d}, Contract Value: {:s}", errInfo.empty() ? "Parameter error detected!" : errInfo, ParamOp->idx, req.value),
                                .references = {ContractPassUtility::getFileReference(CB)},
                            });
                            goto exit_param_analysis;
                        }
                        if (f == Fulfillment::FULFILLED && req.comp == Comparator::EXEQ) {
                            // Parameter fulfills exception value. Stop checking this parameter
                            goto exit_param_analysis;
                        }
                    }
                }
            }
            exit_param_analysis:
            *Expr->Status = resf;
        }
    }

    return PreservedAnalyses::all();
}

std::string createCompErr(const Comparator comp, const ConstantInt* callCI, const ConstantInt* valueCI) {
    SmallString<10> tmpCs;
    callCI->getValue().toStringSigned(tmpCs);
    std::string callCs = tmpCs.c_str();
    tmpCs = "";
    valueCI->getValue().toStringSigned(tmpCs);
    std::string valueCs = tmpCs.c_str();
    switch (comp) {
        case Comparator::NEQ:
            return "Parameter matches forbidden value (" + callCs + ")!";
        case Comparator::EQ:
            return "Parameter does not match required value (" + callCs + ")!";
        case Comparator::GT:
            return "Call parameter value (" + callCs + ") not greater than contract value (" + valueCs + ")!";
        case Comparator::GTEQ:
            return "Call parameter value (" + callCs + ") not greater or equal to contract value (" + valueCs + ")!";
        case Comparator::LT:
            return "Call parameter value (" + callCs + ") not less than contract value (" + valueCs + ")!";
        case Comparator::LTEQ:
            return "Call parameter value (" + callCs + ") not less or equal to contract value (" + valueCs + ")!";
        case Comparator::EXEQ:
            llvm_unreachable("Exception Equal should not trigger an error!");
    }
}

Fulfillment compareCI(const ConstantInt* CI, const ConstantInt* CI2, Comparator comp) {
    switch (comp) {
        case Comparator::NEQ:
            return CI->getValue().getSExtValue() != CI2->getValue().getSExtValue() ? Fulfillment::UNKNOWN : Fulfillment::BROKEN;
        case Comparator::EQ:
            return CI->getValue().getSExtValue() == CI2->getValue().getSExtValue() ? Fulfillment::UNKNOWN : Fulfillment::BROKEN;
        case Comparator::GTEQ:
            return CI->getValue().sge(CI2->getValue()) ? Fulfillment::UNKNOWN : Fulfillment::BROKEN;
        case Comparator::GT:
            return CI->getValue().sgt(CI2->getValue()) ? Fulfillment::UNKNOWN : Fulfillment::BROKEN;
        case Comparator::LTEQ:
            return CI->getValue().sle(CI2->getValue()) ? Fulfillment::UNKNOWN : Fulfillment::BROKEN;
        case Comparator::LT:
            return CI->getValue().slt(CI2->getValue()) ? Fulfillment::UNKNOWN : Fulfillment::BROKEN;
        case Comparator::EXEQ:
            return CI->getValue().getSExtValue() == CI2->getValue().getSExtValue() ? Fulfillment::FULFILLED : Fulfillment::UNKNOWN;
    }
}

Fulfillment ContractVerifierParamPass::checkParamReq(std::set<Value*> vars, CallBase* call, int idx, Comparator const& comp, std::string& ErrInfo) {
    for (Value* var : vars) {
        Value* callVal = call->getArgOperand(idx);
        if (AllocaInst* AI = dyn_cast<AllocaInst>(callVal)) {
            for (Instruction* cur = call->getPrevNode(); cur && !isa<CallInst>(cur); cur = cur->getPrevNode()) {
                if (GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(cur)) {
                    if (GEP->getPointerOperand() != callVal) continue;
                    if (LoadInst* LI = dyn_cast<LoadInst>(cur->getNextNode())) {
                        callVal = LI->getPointerOperand();
                    }
                }
            }
        }
        // Always prefer constint comparisons.
        // For C, check if its a constint inttoptr
        if (ConstantExpr* CE = dyn_cast<ConstantExpr>(callVal)) {
            if (isa<IntToPtrInst>(CE->getAsInstruction())) {
                callVal = CE->getOperand(0);
            }
        }
        // For Fortran, this sometimes requires a lil trickery:
        // First, try to get at the actual value instead of the weird pointer that is passed as the arg in IR
        if (Instruction* I = dyn_cast<Instruction>(callVal)) {
            StoreInst* SI = ContractPassUtility::getLastStore(call, idx, I, &MAM->getResult<FunctionAnalysisManagerModuleProxy>(*I->getModule()).getManager());
            if (SI && isa<ConstantInt>(SI->getValueOperand())) {
                callVal = SI->getValueOperand();
            }
        }
        // Next, for some global vals its just a struct with one constint member, resolve that as well (for both param val and call val)
        std::vector<Value**> tmps = {&callVal, &var};
        for (Value** tmp : tmps) {
            Value* res = ContractPassUtility::fortCheckAndGetGlbInt(*tmp);
            *tmp = res ? res : *tmp; 
        }

        // Check if its a pointer with the weird fortran metadata descriptor. If so, need to get contained value using heuristic
        if (AllocaInst const* AI = dyn_cast<AllocaInst>(callVal)) {
            if (StructType const* T = dyn_cast<StructType>(AI->getAllocatedType())) {
                if (T->getElementType(0) == Basic_Types.Ptr_Type &&
                    T->getElementType(1) == Basic_Types.Int64_Type &&
                    T->getElementType(2) == Basic_Types.Int_Type &&
                    T->getNumElements() == 9) {
                    // Fortran Metadata thing - Find first GEP to callVal
                    Instruction* cur = call->getPrevNode();
                    for (; cur && !isa<CallInst>(cur); cur = cur->getPrevNode()) {
                        if (GetElementPtrInst const* GEP = dyn_cast<GetElementPtrInst>(cur)) {
                            if (GEP->getPointerOperand() != callVal) continue; 
                            if (ExtractValueInst const* EVI = dyn_cast<ExtractValueInst>(GEP->getPrevNode())) {
                                for (Use const& U : EVI->getAggregateOperand()->uses()) {
                                    if (InsertValueInst* IVI = dyn_cast<InsertValueInst>(U.get())) {
                                        if (IVI->getIndices()[0] == 0 && IVI->getNumIndices() == 1) {
                                            callVal = IVI->getInsertedValueOperand();
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (callVal->getType()->isPointerTy()) {
            switch (comp) {
                case Comparator::NEQ:
                    if (ContractPassUtility::checkParamMatch(callVal, var, ParamAccess::NORMAL, MAM)) {
                        ErrInfo = "Parameter matches or is alias to forbidden pointer value!";
                        return Fulfillment::BROKEN;
                    }
                    return Fulfillment::FULFILLED;
                case Comparator::EQ:
                    if (!ContractPassUtility::checkParamMatch(callVal, var, ParamAccess::NORMAL, MAM)) {
                        ErrInfo = "Parameter does not match required pointer value!";
                        return Fulfillment::BROKEN;
                    }
                    return Fulfillment::FULFILLED;
                case Comparator::EXEQ:
                    if (ContractPassUtility::checkParamMatch(callVal, var, ParamAccess::NORMAL, MAM)) {
                        ErrInfo = "Note: Parameter matches or is alias to exception value.";
                        return Fulfillment::FULFILLED;
                    }
                    // Not an exception. Continue analysis, so far no info gained
                    continue;
                default:
                    errs() << "Attempt to compare pointers! Not performing parameter analysis at "
                           << ContractPassUtility::getInstrLocStr(call) << " for index " << idx << "\n";
                    return Fulfillment::UNKNOWN;
            }
        }
        // Ensured that !isPtr, can get constinfo if present
        if (const ConstantInt* callCI = dyn_cast<ConstantInt>(callVal)) {
            if (const ConstantInt* varCI = dyn_cast<ConstantInt>(var)) {
                Fulfillment f = compareCI(callCI, varCI, comp);
                if (f == Fulfillment::BROKEN) {
                    ErrInfo = createCompErr(comp, callCI, varCI);
                    return f;
                } else if (f == Fulfillment::FULFILLED) return f;
            }
        }
    }

    return Fulfillment::UNKNOWN;
}
