#include "ContractVerifierPreCall.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "ContractPassUtility.hpp"

#include <algorithm>
#include <vector>
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

    for (ContractManagerAnalysis::Contract C : DB.Contracts) {
        if (*C.Status == Fulfillment::UNKNOWN && C.Data.Pre.has_value()) {
            // Contract has a precondition
            const ContractExpression& Expr = C.Data.Pre.value();
            std::string err;
            bool result = false;
            switch (Expr.OP->type()) {
                case OperationType::CALL: {
                    const CallOperation& cOP = dynamic_cast<const CallOperation&>(*Expr.OP);
                    result = checkPreCall(cOP, C.F, M, err) == CallStatusVal::CALLED;
                    break;
                }
                default: continue;
            }
            if (!err.empty()) {
                errs() << err << "\n";
            }
            if (result) {
                *C.Status = Fulfillment::FULFILLED;
            } else {
                *C.Status = Fulfillment::BROKEN;
            }
        }
    }

    return PreservedAnalyses::all();
}

struct IterTypePreCall {
    const std::string Target;
    const Function* F;
    const std::vector<int> reqParams;
};

ContractVerifierPreCallPass::CallStatus transferCallStat(ContractVerifierPreCallPass::CallStatus cur, const Instruction* I, void* data) {
    if (cur.CurVal == ContractVerifierPreCallPass::CallStatusVal::ERROR) return cur;
    if (cur.CurVal == ContractVerifierPreCallPass::CallStatusVal::CALLED) return cur;

    IterTypePreCall* Data = static_cast<IterTypePreCall*>(data);
    if (const CallBase* CB = dyn_cast<CallBase>(I)) {
        if (CB->getCalledFunction()->getName() == Data->Target) {
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
                cur.CurVal = ContractVerifierPreCallPass::CallStatusVal::ERROR;
                return cur;
            }
            for (int x : Data->reqParams) {
                for (const CallBase* Candidate : cur.candidate) {
                    for (const Value* candidateParam : Candidate->operand_values()) {
                        if (candidateParam == CB->getArgOperand(x)) {
                            // Success!
                            cur.CurVal = ContractVerifierPreCallPass::CallStatusVal::CALLED;
                            return cur;
                        }
                    }
                }
            }
            // Any required parameter not used by any candidate
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

ContractVerifierPreCallPass::CallStatusVal ContractVerifierPreCallPass::checkPreCall(const CallOperation& cOP, const Function* F, const Module& M, std::string& error) {
    const Function* mainF = M.getFunction("main");
    if (!mainF) {
        error = "Cannot find main function, cannot construct path to check precall!";
        return CallStatusVal::ERROR;
    }
    const Instruction* Entry = mainF->getEntryBlock().getFirstNonPHI();

    IterTypePreCall data = { cOP.Function, F, cOP.Params };
    CallStatus init = { CallStatusVal::NOTCALLED, {}};
    std::map<const Instruction *, CallStatus> AnalysisInfo = GenericWorklist<CallStatus>(Entry, transferCallStat, mergeCallStat, &data, init);

    // Take intersection of all returning instructions
    CallStatusVal res = CallStatusVal::CALLED;
    for (const User* U : F->users()) {
        if (const CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledFunction() == F) {
                res = std::max(AnalysisInfo[CB].CurVal, res);
            }
        }
    }
    return res;
}
