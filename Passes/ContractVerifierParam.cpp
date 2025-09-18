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
#include <llvm/IR/Type.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
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
            Fulfillment resf = Fulfillment::FULFILLED;

            // Perform the check on each callsite
            for (const User* U : C.F->users()) {
                if (const CallBase* CB = dyn_cast<CallBase>(U)) {
                    for (std::pair<const Comparator, const std::string> req : ParamOp->reqs) {
                        // Figure out value(s) to check against
                        Value* var;
                        try {
                            // First, check if constant value provided
                            int ivalue = std::stoi(req.second);
                            var = ConstantInt::get(Type::getInt64Ty(M.getContext()), ivalue);
                        } catch(std::exception& e) {
                            // Otherwise, check against value database
                            if (!DB.ContractVariableData.contains(req.second)) {
                                errs() << "Undefined non-constint contract value identifier \"" << req.second << "\"!\n";
                                errs() << "Requirement will not be analysed!\n";
                                continue;
                            }
                            var = DB.ContractVariableData[req.second];
                        }

                        // Perform check
                        std::string errInfo = "";
                        Fulfillment f = checkParamReq(var, CB->getArgOperand(ParamOp->idx), req.first, errInfo);
                        if (f == Fulfillment::BROKEN) {
                            resf = Fulfillment::BROKEN;
                                Expr->ErrorInfo->push_back({
                                .error_id = "Param",
                                .text = std::format("{:s} Parameter Index: {:d}, Contract Value: {:s}", errInfo.empty() ? "Parameter error detected!" : errInfo, ParamOp->idx, req.second),
                                .references = {ContractPassUtility::getFileReference(CB)},
                            });
                            goto exit_param_analysis;
                        }
                        if (f == Fulfillment::FULFILLED && req.first == Comparator::EXEQ) {
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
            return CI->getValue().getSExtValue() != CI2->getValue().getSExtValue() ? Fulfillment::FULFILLED : Fulfillment::BROKEN;
        case Comparator::GTEQ:
            return CI->getValue().sge(CI2->getValue()) ? Fulfillment::FULFILLED : Fulfillment::BROKEN;
        case Comparator::GT:
            return CI->getValue().sgt(CI2->getValue()) ? Fulfillment::FULFILLED : Fulfillment::BROKEN;
        case Comparator::LTEQ:
            return CI->getValue().sle(CI2->getValue()) ? Fulfillment::FULFILLED : Fulfillment::BROKEN;
        case Comparator::LT:
            return CI->getValue().slt(CI2->getValue()) ? Fulfillment::FULFILLED : Fulfillment::BROKEN;
        case Comparator::EXEQ:
            return CI->getValue().getSExtValue() == CI2->getValue().getSExtValue() ? Fulfillment::FULFILLED : Fulfillment::UNKNOWN;
    }
}

Fulfillment ContractVerifierParamPass::checkParamReq(const Value* var, const Value* callVal, Comparator comp, std::string& ErrInfo) {
    if (callVal->getType()->isPointerTy()) {
        switch (comp) {
            case Comparator::NEQ:
                if (ContractPassUtility::checkParamMatch(callVal, var, ParamAccess::NORMAL, MAM)) {
                    ErrInfo = "Parameter matches or is alias to forbidden pointer value!";
                    return Fulfillment::BROKEN;
                }
                return Fulfillment::FULFILLED;
            case Comparator::EXEQ:
                if (ContractPassUtility::checkParamMatch(callVal, var, ParamAccess::NORMAL, MAM)) {
                    ErrInfo = "Note: Parameter matches or is alias to exception value.";
                    return Fulfillment::FULFILLED;
                }
                // Not an exception. Continue analysis, so far no info gained
                return Fulfillment::UNKNOWN;
            default:
                errs() << "Attempt to compare pointers! Not performing parameter analysis\n";
                return Fulfillment::UNKNOWN;
        }
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
