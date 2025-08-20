#include "ContractVerifierPostCall.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "ContractPassUtility.hpp"
#include "ErrorMessage.h"

#include <algorithm>
#include <llvm/Demangle/Demangle.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <utility>
#include <vector>
#include <sstream>
#include <llvm/Analysis/InlineCost.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>

using namespace llvm;
using namespace ContractTree;

PreservedAnalyses ContractVerifierPostCallPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    ContractManagerAnalysis::ContractDatabase DB = AM.getResult<ContractManagerAnalysis>(M);
    Tags = DB.Tags;
    for (ContractManagerAnalysis::LinearizedContract const& C : DB.LinearizedContracts) {
        for (const std::shared_ptr<ContractExpression> Expr : C.Post) {
            if (*Expr->Status != Fulfillment::UNKNOWN) continue;
            // Contract has a precondition
            std::string err;
            CallStatus result;
            switch (Expr->OP->type()) {
                case OperationType::CALL:
                case OperationType::CALLTAG: {
                    const CallOperation* cOP = static_cast<const CallOperation*>(Expr->OP.get());
                    C.DebugInfo->push_back("[ContractVerifierPostCall] Attempting to verify expression: " + Expr->ExprStr);
                    result = checkPostCall(cOP, C, *Expr, cOP->type() == OperationType::CALLTAG, M, err);
                    break;
                }
                default: continue;
            }
            if (!err.empty()) {
                errs() << err << "\n";
            }
            *Expr->Status = result == CallStatus::CALLED ? Fulfillment::FULFILLED : Fulfillment::BROKEN;
        }
    }

    return PreservedAnalyses::all();
}

void ContractVerifierPostCallPass::appendDebugStr(std::string Target, bool isTag, const CallBase* Provider, const std::set<const CallBase *> candidates, std::vector<ErrorMessage>& err) {
    // Generic error message
    err.push_back({
        .error_id = "PostCall",
        .text = "[ContractVerifierPostCall] Did not find postcall function " + Target + (isTag ? " (Tag)" : "") + " with required parameters after "
                    + demangle(Provider->getCalledFunction()->getName()) + " at " + ContractPassUtility::getInstrLocStr(Provider),
        .references = {ContractPassUtility::getFileReference(Provider)},
    });
    // err.push_back("[ContractVerifierPostCall] Did not find postcall function " + Target + (isTag ? " (Tag)" : "") + " with required parameters after "
    //                 + demangle(Provider->getCalledFunction()->getName()) + " at " + ContractPassUtility::getInstrLocStr(Provider));
    // if (!candidates.empty()) {
    //     // There were candidates, none fit
    //     for (const CallBase* CB : candidates)
    //         err.push_back("[ContractVerifierPostCall] Unfitting Candidate: " + demangle(CB->getCalledFunction()->getName()) + " at " + ContractPassUtility::getInstrLocStr(CB));
    // } else {
    //     // No candidates at all
    //     err.push_back("[ContractVerifierPostCall] No candidates were found.");
    // }
}

struct IterTypePostCall {
    std::set<const CallBase*> dbg_candidates;
    std::vector<std::string> dbg;
    const std::string Target;
    const CallBase* callsite;
    const std::vector<CallParam> reqParams;
    const bool isTag;
    std::map<const Function*, std::vector<TagUnit>> Tags;
};

ContractVerifierPostCallPass::CallStatus transferPostCallStat(ContractVerifierPostCallPass::CallStatus cur, const Instruction* I, void* data) {
    if (cur == ContractVerifierPostCallPass::CallStatus::CALLED) return cur;

    IterTypePostCall* Data = static_cast<IterTypePostCall*>(data);
    if (const CallBase* CB = dyn_cast<CallBase>(I)) {
        if (ContractPassUtility::checkCalledApplies(CB, Data->Target, Data->isTag, Data->Tags)) {
            if (Data->reqParams.empty()) {
                cur = ContractVerifierPostCallPass::CallStatus::CALLED;
                return cur;
            }
            // Target function was called, need to check the parameters
            // Paramcheck is resolved once target found
            for (CallParam P : Data->reqParams) {
                if (ContractPassUtility::checkCallParamApplies(Data->callsite, CB, Data->Target, P, Data->Tags)) {
                    // Success!
                    return ContractVerifierPostCallPass::CallStatus::CALLED;
                }
            }
            // Missing required parameter... Add to debuginfo
            Data->dbg_candidates.insert(CB);
        }
    }
    // Not a call. Just forward info
    return cur;
}

std::pair<ContractVerifierPostCallPass::CallStatus,bool> mergePostCallStat(ContractVerifierPostCallPass::CallStatus prev, ContractVerifierPostCallPass::CallStatus cur, const Instruction* I, void* data) {
    ContractVerifierPostCallPass::CallStatus cs = std::max(prev, cur);
    if ((prev == ContractVerifierPostCallPass::CallStatus::CALLED || cur == ContractVerifierPostCallPass::CallStatus::CALLED) &&
         cs != ContractVerifierPostCallPass::CallStatus::CALLED) {
        IterTypePostCall* Data = static_cast<IterTypePostCall*>(data);
        Data->dbg.push_back("[ContractVerifierPostCall] NOTE: Successful fulfillment was lost at " + ContractPassUtility::getInstrLocStr(I) + " due to merging of different branches.");
    }
    return { cs, cs > prev };
}

ContractVerifierPostCallPass::CallStatus ContractVerifierPostCallPass::checkPostCall(const CallOperation* cOP, const ContractManagerAnalysis::LinearizedContract& C, ContractExpression const& Expr, const bool isTag, const Module& M, std::string& error) {
    IterTypePostCall data = { {}, {}, cOP->Function, nullptr, cOP->Params, isTag, Tags };

    for (const User* U : C.F->users()) {
        if (const CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledFunction() == C.F) {
                data.callsite = CB;
                std::map<const Instruction *, CallStatus> AnalysisInfo = ContractPassUtility::GenericWorklist<CallStatus>(CB->getNextNode(), transferPostCallStat, mergePostCallStat, &data, CallStatus::NOTCALLED);
                C.DebugInfo->insert(C.DebugInfo->end(), data.dbg.begin(), data.dbg.end());
                for (std::pair<const Instruction *, CallStatus> x : AnalysisInfo) {
                    if (isa<ReturnInst>(x.first) && x.first->getParent()->getParent()->getName() == "main" && x.second == CallStatus::NOTCALLED) {
                        appendDebugStr(cOP->Function, isTag, data.callsite, data.dbg_candidates, *Expr.ErrorInfo);
                        return CallStatus::NOTCALLED;
                    }
                }
            }
        }
    }
    return CallStatus::CALLED;
}
