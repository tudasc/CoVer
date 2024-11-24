#include "ContractVerifierPostCall.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "ContractPassUtility.hpp"

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
#include <llvm/Transforms/Instrumentation.h>

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
                    const CallOperation* cOP = dynamic_cast<const CallOperation*>(Expr->OP.get());
                    result = checkPostCall(cOP, C, cOP->type() == OperationType::CALLTAG, M, err);
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

std::string ContractVerifierPostCallPass::createDebugStr(const CallBase* Provider) {
    std::stringstream str;
    str << "[ContractVerifierPostCall] Did not find postcall function with required parameters for "
        << demangle(Provider->getCalledFunction()->getName()) << " at "
        << ContractPassUtility::getInstrLocStr(Provider) << "\n";
    return str.str();
}

struct IterTypePostCall {
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
            // Missing required parameter...
        }
    }
    // Not a call. Just forward info
    return cur;
}

std::pair<ContractVerifierPostCallPass::CallStatus,bool> mergePostCallStat(ContractVerifierPostCallPass::CallStatus prev, ContractVerifierPostCallPass::CallStatus cur, const Instruction* I, void* data) {
    ContractVerifierPostCallPass::CallStatus cs = std::max(prev, cur);
    return { cs, cs > prev };
}

ContractVerifierPostCallPass::CallStatus ContractVerifierPostCallPass::checkPostCall(const CallOperation* cOP, const ContractManagerAnalysis::LinearizedContract& C, const bool isTag, const Module& M, std::string& error) {
    IterTypePostCall data = { {}, cOP->Function, nullptr, cOP->Params, isTag, Tags };

    for (const User* U : C.F->users()) {
        if (const CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledFunction() == C.F) {
                data.callsite = CB;
                std::map<const Instruction *, CallStatus> AnalysisInfo = ContractPassUtility::GenericWorklist<CallStatus>(CB->getNextNode(), transferPostCallStat, mergePostCallStat, &data, CallStatus::NOTCALLED);
                C.DebugInfo->insert(C.DebugInfo->end(), data.dbg.begin(), data.dbg.end());
                for (std::pair<const Instruction *, CallStatus> x : AnalysisInfo) {
                    if (isa<ReturnInst>(x.first) && x.first->getParent()->getParent()->getName() == "main" && x.second == CallStatus::NOTCALLED) return CallStatus::NOTCALLED;
                }
            }
        }
    }
    return CallStatus::CALLED;
}
