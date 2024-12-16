#include "ContractVerifierPreCall.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "ContractPassUtility.hpp"

#include <algorithm>
#include <llvm/Demangle/Demangle.h>
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

PreservedAnalyses ContractVerifierPreCallPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    ContractManagerAnalysis::ContractDatabase DB = AM.getResult<ContractManagerAnalysis>(M);
    Tags = DB.Tags;

    for (ContractManagerAnalysis::LinearizedContract const& C : DB.LinearizedContracts) {
        for (const std::shared_ptr<ContractExpression> Expr : C.Pre) {
            if (*Expr->Status != Fulfillment::UNKNOWN) continue;
            // Contract has a precondition
            std::string err;
            CallStatusVal result;
            switch (Expr->OP->type()) {
                case OperationType::CALL:
                case OperationType::CALLTAG: {
                    const CallOperation* cOP = dynamic_cast<const CallOperation*>(Expr->OP.get());
                    C.DebugInfo->push_back("[ContractVerifierPreCall] Attempting to verify expression: " + Expr->ExprStr);
                    result = checkPreCall(cOP, C, *Expr, cOP->type() == OperationType::CALLTAG, M, err);
                    break;
                }
                default: continue;
            }
            if (!err.empty()) {
                errs() << err << "\n";
            }
            if (result < CallStatusVal::ERROR) {
                *Expr->Status = Fulfillment::FULFILLED;
            } else {
                *Expr->Status = Fulfillment::BROKEN;
            }
        }
    }

    return PreservedAnalyses::all();
}

void ContractVerifierPreCallPass::appendDebugStr(std::string Target, bool isTag, const CallBase* Provider, const std::set<const CallBase *> candidates, std::vector<std::string>& err) {
    // Generic error message
    err.push_back("[ContractVerifierPreCall] Did not find precall function " + Target + (isTag ? " (Tag)" : "") + " with required parameters before "
                    + demangle(Provider->getCalledFunction()->getName()) + " at " + ContractPassUtility::getInstrLocStr(Provider));
    if (!candidates.empty()) {
        // There were candidates, none fit
        for (const CallBase* CB : candidates)
            err.push_back("[ContractVerifierPreCall] Unfitting Candidate: " + demangle(CB->getCalledFunction()->getName()) + " at " + ContractPassUtility::getInstrLocStr(CB));
    } else {
        // No candidates at all
        err.push_back("[ContractVerifierPreCall] No candidates were found.");
    }
}

struct IterTypePreCall {
    std::vector<std::string> err;
    std::vector<std::string> dbg;
    const std::string Target;
    const Function* F;
    const std::vector<CallParam> reqParams;
    const bool isTag;
    std::map<const Function*, std::vector<TagUnit>> Tags;
};

ContractVerifierPreCallPass::CallStatus transferPreCallStat(ContractVerifierPreCallPass::CallStatus cur, const Instruction* I, void* data) {
    if (cur.CurVal == ContractVerifierPreCallPass::CallStatusVal::ERROR) return cur;
    if (cur.CurVal == ContractVerifierPreCallPass::CallStatusVal::CALLED) return cur;

    IterTypePreCall* Data = static_cast<IterTypePreCall*>(data);
    if (const CallBase* CB = dyn_cast<CallBase>(I)) {
        if (ContractPassUtility::checkCalledApplies(CB, Data->Target, Data->isTag, Data->Tags)) {
            if (Data->reqParams.empty()) {
                cur.CurVal = ContractVerifierPreCallPass::CallStatusVal::CALLED;
                return cur;
            }
            // Target function was called, need to check the parameters
            cur.CurVal = llvm::ContractVerifierPreCallPass::CallStatusVal::PARAMCHECK;
            cur.candidate.insert(CB);
            // Paramcheck is resolved once target found
            return cur;
        }
        if (CB->getCalledFunction() == Data->F) {
            // Found contract supplier. Either paramcheck or error
            if (cur.CurVal != ContractVerifierPreCallPass::CallStatusVal::PARAMCHECK) {
                ContractVerifierPreCallPass::appendDebugStr(Data->Target, Data->isTag, CB, cur.candidate, Data->err);
                cur.CurVal = ContractVerifierPreCallPass::CallStatusVal::ERROR;
                return cur;
            }
            for (CallParam param : Data->reqParams) {
                for (const CallBase* Candidate : cur.candidate) {
                    if (ContractPassUtility::checkCallParamApplies(CB, Candidate, Data->Target, param, Data->Tags)) {
                        // Success!
                        cur.CurVal = ContractVerifierPreCallPass::CallStatusVal::CALLED;
                        return cur;
                    }
                }
            }
            // Any required parameter not used by any candidate
            ContractVerifierPreCallPass::appendDebugStr(Data->Target, Data->isTag, CB, cur.candidate, Data->err);
            cur.CurVal = ContractVerifierPreCallPass::CallStatusVal::ERROR;
            return cur;
        }
    }
    // Not a call. Just forward info
    return cur;
}

std::pair<ContractVerifierPreCallPass::CallStatus,bool> mergePreCallStat(ContractVerifierPreCallPass::CallStatus prev, ContractVerifierPreCallPass::CallStatus cur, const Instruction* I, void* data) {
    // Intersection of candidates
    std::set<const CallBase*> intersect;
    std::set_intersection(prev.candidate.begin(), prev.candidate.end(), cur.candidate.begin(), cur.candidate.end(),
                 std::inserter(intersect, intersect.begin()));
    ContractVerifierPreCallPass::CallStatus cs;
    cs.candidate = intersect;
    cs.CurVal = std::max(prev.CurVal, cur.CurVal);
    if ((prev.CurVal == ContractVerifierPreCallPass::CallStatusVal::CALLED || cur.CurVal == ContractVerifierPreCallPass::CallStatusVal::CALLED) &&
         cs.CurVal != ContractVerifierPreCallPass::CallStatusVal::CALLED) {
        IterTypePreCall* Data = static_cast<IterTypePreCall*>(data);
        Data->dbg.push_back("[ContractVerifierPreCall] NOTE: Successful fulfillment was lost at " + ContractPassUtility::getInstrLocStr(I) + " due to merging of different branches.");
    }
    return { cs, cs.CurVal > prev.CurVal };
}

ContractVerifierPreCallPass::CallStatusVal ContractVerifierPreCallPass::checkPreCall(const CallOperation* cOP, ContractManagerAnalysis::LinearizedContract const& C, ContractExpression const& Expr, const bool isTag, const Module& M, std::string& error) {
    const Function* mainF = M.getFunction("main");
    if (!mainF) {
        error = "Cannot find main function, cannot construct path to check precall!";
        return CallStatusVal::ERROR;
    }
    const Instruction* Entry = mainF->getEntryBlock().getFirstNonPHI();

    IterTypePreCall data = { {}, {}, cOP->Function, C.F, cOP->Params, isTag, Tags };
    CallStatus init = { CallStatusVal::NOTCALLED, {}};
    std::map<const Instruction *, CallStatus> AnalysisInfo = ContractPassUtility::GenericWorklist<CallStatus>(Entry, transferPreCallStat, mergePreCallStat, &data, init);

    C.DebugInfo->insert(C.DebugInfo->end(), data.dbg.begin(), data.dbg.end());
    Expr.ErrorInfo->insert(Expr.ErrorInfo->end(), data.err.begin(), data.err.end());

    // Take max over all analysis info
    // Correct usage will not contain error
    CallStatusVal res = CallStatusVal::CALLED;
    for (std::pair<const Instruction*, CallStatus> AI : AnalysisInfo) {
        if (const CallBase* CB = dyn_cast<CallBase>(AI.first)) {
            if (CB->getCalledFunction() == C.F) {
                res = std::max(AI.second.CurVal, res);
            }
        }
    }
    return res;
}
