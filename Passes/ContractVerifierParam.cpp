#include "ContractVerifierParam.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "ContractPassUtility.hpp"

#include <format>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>
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

using namespace llvm;
using namespace ContractTree;

PreservedAnalyses ContractVerifierParamPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    ContractManagerAnalysis::ContractDatabase DB = AM.getResult<ContractManagerAnalysis>(M);
    MAM = &AM;
    for (ContractManagerAnalysis::LinearizedContract const& C : DB.LinearizedContracts) {
        for (const std::shared_ptr<ContractExpression> Expr : C.Pre) {
            if (*Expr->Status != Fulfillment::UNKNOWN) continue;
            // Contract has a precondition
            std::string err;
            if (Expr->OP->type() != OperationType::PARAM) continue;
            const ParamOperation* ParamOp = dynamic_cast<const ParamOperation*>(Expr->OP.get());
            C.DebugInfo->push_back("[ContractVerifierParam] Attempting to verify expression: " + Expr->ExprStr);
            for (std::pair<const Comparator, const std::string> req : ParamOp->reqs) {
                // First, check against value database
                if (!DB.ContractVariableData.contains(req.second)) {
                    errs() << "Undefined contract value identifier \"" << req.second << "\"!\n";
                    errs() << "Requirement will not be analysed!\n";
                    errs() << DB.ContractVariableData.begin()->first << " " << req.second << "\n";
                    errs() << DB.ContractVariableData.begin()->first.length() << " " << req.second.length() << "\n";
                    continue;
                }
                // Perform the check on each callsite
                for (const User* U : C.F->users()) {
                    if (const CallBase* CB = dyn_cast<CallBase>(U)) {
                        std::string errInfo = "";
                        Fulfillment f = checkParamReq(DB.ContractVariableData[req.second].first, CB->getArgOperand(ParamOp->idx), req.first, DB.ContractVariableData[req.second].second, errInfo);
                        if (f == Fulfillment::BROKEN) {
                            *Expr->Status = Fulfillment::BROKEN;
                            if (!errInfo.empty()) {
                                    Expr->ErrorInfo->push_back({
                                    .error_id = "Param",
                                    .text = errInfo + std::format("\nParameter Index: {:d}\nContract Value Name: {:s}", ParamOp->idx, req.second),
                                    .references = {ContractPassUtility::getFileReference(CB)},
                                });
                            }
                        }
                    }
                }
            }
            if (*Expr->Status == Fulfillment::UNKNOWN) *Expr->Status = Fulfillment::FULFILLED;
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
        case Comparator::GT:
            return "Call parameter value (" + callCs + ") not greater than contract value (" + valueCs + ")!";
        case Comparator::GTEQ:
            return "Call parameter value (" + callCs + ") not greater or equal to contract value (" + valueCs + ")!";
        case Comparator::LT:
            return "Call parameter value (" + callCs + ") not less than contract value (" + valueCs + ")!";
        case Comparator::LTEQ:
            return "Call parameter value (" + callCs + ") not less or equal to contract value (" + valueCs + ")!";
    }
}

Fulfillment compareCI(const ConstantInt* CI, const ConstantInt* CI2, Comparator comp) {
    switch (comp) {
        case Comparator::NEQ:
            return !APInt::isSameValue(CI->getValue(), CI2->getValue()) ? Fulfillment::FULFILLED : Fulfillment::BROKEN;
        case Comparator::GTEQ:
            return CI->getValue().sge(CI2->getValue()) ? Fulfillment::FULFILLED : Fulfillment::BROKEN;
        case Comparator::GT:
            return CI->getValue().sgt(CI2->getValue()) ? Fulfillment::FULFILLED : Fulfillment::BROKEN;
        case Comparator::LTEQ:
            return CI->getValue().sle(CI2->getValue()) ? Fulfillment::FULFILLED : Fulfillment::BROKEN;
        case Comparator::LT:
            return CI->getValue().slt(CI2->getValue()) ? Fulfillment::FULFILLED : Fulfillment::BROKEN;
    }
}

Fulfillment ContractVerifierParamPass::checkParamReq(const Value* var, const Value* callVal, Comparator comp, bool isPtr, std::string& ErrInfo) {
    if (comp == Comparator::NEQ && isPtr) {
        if (ContractPassUtility::checkParamMatch(callVal, var, ParamAccess::NORMAL, MAM)) {
            ErrInfo = "Parameter matches or is alias to forbidden pointer value!";
            return Fulfillment::BROKEN;
        }
        #warning TODO maybe unknown? Depends on analysis confidence
        return Fulfillment::FULFILLED;
    } else if (comp != Comparator::NEQ && isPtr) {
        errs() << "Attempt to compare pointers! Not performing analysis\n";
        return Fulfillment::UNKNOWN;
    }
    // Ensured that !isPtr, can get constinfo if present
    if (const ConstantInt* callCI = dyn_cast<ConstantInt>(callVal)) {
        if (const ConstantInt* varCI = dyn_cast<ConstantInt>(var)) {
            Fulfillment f = compareCI(callCI, varCI, comp);
            if (f == Fulfillment::BROKEN) {
                ErrInfo = createCompErr(comp, callCI, varCI);
            }
            return f;
        }
    }

    return Fulfillment::UNKNOWN;
}
