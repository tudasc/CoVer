#include "ContractVerifierPreCall.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "ContractPassUtility.hpp"

#include <algorithm>
#include <llvm/Demangle/Demangle.h>
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

    for (ContractManagerAnalysis::Contract C : DB.Contracts) {
        if (C.Data.Pre.has_value() && *C.Data.Pre->Status == Fulfillment::UNKNOWN) {
            // Contract has a precondition
            const ContractExpression& Expr = C.Data.Pre.value();
            std::string err;
            CallStatusVal result;
            switch (Expr.OP->type()) {
                case OperationType::CALL: {
                    const CallOperation& cOP = dynamic_cast<const CallOperation&>(*Expr.OP);
                    result = checkPreCall(cOP, C, false, M, err);
                    break;
                }
                case OperationType::CALLTAG: {
                    const CallTagOperation& ctOP = dynamic_cast<const CallTagOperation&>(*Expr.OP);
                    result = checkPreCall(ctOP, C, true, M, err);
                    break;
                }
                default: continue;
            }
            if (!err.empty()) {
                errs() << err << "\n";
            }
            if (result < CallStatusVal::ERROR) {
                *Expr.Status = Fulfillment::FULFILLED;
            } else {
                *Expr.Status = Fulfillment::BROKEN;
            }
        }
    }

    return PreservedAnalyses::all();
}

std::string ContractVerifierPreCallPass::createDebugStr(const CallBase* Provider, const std::set<const CallBase *> candidates) {
    std::stringstream str;
    str << "[ContractVerifierPreCall] Did not find precall function with required parameters before "
        << demangle(Provider->getCalledFunction()->getName()) << " at "
        << getInstrLocStr(Provider)
        << "\nUnfitting candidates:";
    for (const CallBase* CB : candidates) {
        str << CB->getCalledFunction()->getName().str() << " at " << getInstrLocStr(CB) << "\n";
    }
    return str.str();
}

struct IterTypePreCall {
    std::vector<std::string> dbg;
    const std::string Target;
    const Function* F;
    const std::vector<CallParam> reqParams;
    const bool isTag;
    std::map<const Function*, std::vector<TagUnit>> Tags;
};

ContractVerifierPreCallPass::CallStatus transferCallStat(ContractVerifierPreCallPass::CallStatus cur, const Instruction* I, void* data) {
    if (cur.CurVal == ContractVerifierPreCallPass::CallStatusVal::ERROR) return cur;
    if (cur.CurVal == ContractVerifierPreCallPass::CallStatusVal::CALLED) return cur;

    IterTypePreCall* Data = static_cast<IterTypePreCall*>(data);
    if (const CallBase* CB = dyn_cast<CallBase>(I)) {
        if (checkCalledApplies(CB, Data->Target, Data->isTag, Data->Tags)) {
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
                Data->dbg.push_back(ContractVerifierPreCallPass::createDebugStr(CB, cur.candidate));
                cur.CurVal = ContractVerifierPreCallPass::CallStatusVal::ERROR;
                return cur;
            }
            for (CallParam param : Data->reqParams) {
                for (const CallBase* Candidate : cur.candidate) {
                    if (checkCallParamApplies(CB, Candidate, Data->Target, param, Data->Tags)) {
                        // Success!
                        cur.CurVal = ContractVerifierPreCallPass::CallStatusVal::CALLED;
                        return cur;
                    }
                }
            }
            // Any required parameter not used by any candidate
            Data->dbg.push_back(ContractVerifierPreCallPass::createDebugStr(CB, cur.candidate));
            cur.CurVal = ContractVerifierPreCallPass::CallStatusVal::ERROR;
            return cur;
        }
    }
    // Not a call. Just forward info
    return cur;
}

ContractVerifierPreCallPass::CallStatus mergeCallStat(ContractVerifierPreCallPass::CallStatus prev, ContractVerifierPreCallPass::CallStatus cur, const Instruction* I, void* data) {
    // Intersection of candidates
    std::set<const CallBase*> intersect;
    std::set_intersection(prev.candidate.begin(), prev.candidate.end(), cur.candidate.begin(), cur.candidate.end(),
                 std::inserter(intersect, intersect.begin()));
    ContractVerifierPreCallPass::CallStatus cs;
    cs.candidate = intersect;
    cs.CurVal = std::max(prev.CurVal, cur.CurVal);
    return cs;
}

ContractVerifierPreCallPass::CallStatusVal ContractVerifierPreCallPass::checkPreCall(const CallOperation& cOP, const ContractManagerAnalysis::Contract& C, const bool isTag, const Module& M, std::string& error) {
    const Function* mainF = M.getFunction("main");
    if (!mainF) {
        error = "Cannot find main function, cannot construct path to check precall!";
        return CallStatusVal::ERROR;
    }
    const Instruction* Entry = mainF->getEntryBlock().getFirstNonPHI();

    IterTypePreCall data = { {}, cOP.Function, C.F, cOP.Params, isTag, Tags };
    CallStatus init = { CallStatusVal::NOTCALLED, {}};
    std::map<const Instruction *, CallStatus> AnalysisInfo = GenericWorklist<CallStatus>(Entry, transferCallStat, mergeCallStat, &data, init);

    C.DebugInfo->insert(C.DebugInfo->end(), data.dbg.begin(), data.dbg.end());

    // Take max over all analysis info
    // Correct usage will not contain error
    CallStatusVal res = CallStatusVal::CALLED;
    for (std::pair<const Instruction*, CallStatus> AI : AnalysisInfo) {
        res = std::max(AI.second.CurVal, res);
    }
    return res;
}
