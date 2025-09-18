#include "ContractVerifierAlloc.hpp"

#include "ContractPassUtility.hpp"
#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include <functional>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/Support/Casting.h>

using namespace llvm;
using namespace ContractTree;

PreservedAnalyses ContractVerifierAllocPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    DB = &AM.getResult<ContractManagerAnalysis>(M);
    MAM = &AM;

    // First, build list of all allocating funcs
    for (ContractManagerAnalysis::LinearizedContract const& C : DB->LinearizedContracts) {
        for (const std::shared_ptr<ContractExpression> Expr : C.Post) {
            if (Expr->OP->type() != OperationType::ALLOC) continue;
            const AllocOperation* AllocOp = dynamic_cast<const AllocOperation*>(Expr->OP.get());
            if (!AllocFuncs.contains(C.F)) AllocFuncs[C.F] = {};
            AllocFuncs[C.F].insert(AllocOp);
            *Expr->Status = Fulfillment::FULFILLED; // Always fulfilled.
        }
    }

    // Now, do analysis
    for (ContractManagerAnalysis::LinearizedContract const& C : DB->LinearizedContracts) {
        for (const std::shared_ptr<ContractExpression> Expr : C.Pre) {
            if (*Expr->Status != Fulfillment::UNKNOWN) continue;
            // Contract has a precondition
            if (Expr->OP->type() != OperationType::ALLOC) continue;
            const AllocOperation* AllocOp = dynamic_cast<const AllocOperation*>(Expr->OP.get());
            C.DebugInfo->push_back("[ContractVerifierAlloc] Attempting to verify expression: " + Expr->ExprStr);
            std::string err;
            AllocStatusVal val = checkAllocReq(AllocOp, M, C.F, err);
            if (!err.empty()) {
                errs() << err << "\n";
                *Expr->Status = Fulfillment::BROKEN;
                return PreservedAnalyses::all();
            }
            *Expr->Status = val == AllocStatusVal::ERROR ? Fulfillment::BROKEN : Fulfillment::FULFILLED;
        }
    }
    return PreservedAnalyses::all();
}

struct IterTypeAlloc {
    std::vector<std::string> err;
    std::vector<std::string> dbg;
    int param;
    ParamAccess acc;
    const Function* F;
};

ContractVerifierAllocPass::AllocStatus ContractVerifierAllocPass::transferAllocStat(AllocStatus cur, const Instruction* I, void* data) {
    if (cur.CurVal == AllocStatusVal::ERROR) return cur;
    if (cur.CurVal == AllocStatusVal::ALLOC) return cur;

    IterTypeAlloc* Data = static_cast<IterTypeAlloc*>(data);

    // Propagate allocations
    if (const StoreInst* SI = dyn_cast<StoreInst>(I)) {
        if (cur.candidate.contains(SI->getValueOperand())) {
            cur.candidate[SI->getPointerOperand()] = ParamAccess::ADDROF;
        }
    }

    if (const CallBase* CB = dyn_cast<CallBase>(I)) {
        if (AllocFuncs.contains(CB->getCalledFunction())) {
            for (const AllocOperation* alloc : AllocFuncs[CB->getCalledFunction()]) {
                #warning TODO different access patterns
                if (alloc->contrP == 99) cur.candidate[CB] = alloc->contrParamAccess;
                else cur.candidate[CB->getArgOperand(alloc->contrP)] = alloc->contrParamAccess;
            }
            // Dont return here! Maybe it also is contr sup
        }
        if (CB->getCalledFunction() == Data->F) {
            // Found contract supplier. Check if param is allocated
            if (ContractPassUtility::isTrivialAlloc(CB->getArgOperand(Data->param))) {
                cur.CurVal = AllocStatusVal::ALLOC;
                return cur;
            }
            // Not trivial, check if explicitly allocated
            for (std::pair<const Value*,ParamAccess> Candidate : cur.candidate) {
                if (ContractPassUtility::checkParamMatch(CB->getArgOperand(Data->param), Candidate.first, Candidate.second, MAM)) {
                    // Success!
                    cur.CurVal = AllocStatusVal::ALLOC;
                    return cur;
                }
            }
            // Any required parameter not used by any candidate
            //appendDebugStr(Data->Target, Data->isTag, CB, cur.candidate, Data->err);
            cur.CurVal = AllocStatusVal::ERROR;
            return cur;
        }
    }
    // Not a call. Just forward info
    return cur;
}

std::pair<ContractVerifierAllocPass::AllocStatus,bool> ContractVerifierAllocPass::mergeAllocStat(AllocStatus prev, AllocStatus cur, const Instruction* I, void* data) {
    std::map<const Value*,ParamAccess> intersect;
    std::set_intersection(prev.candidate.begin(), prev.candidate.end(), cur.candidate.begin(), cur.candidate.end(),
                 std::inserter(intersect, intersect.begin()));

    AllocStatus merge = { std::max(prev.CurVal, cur.CurVal), intersect };
    
    return { merge, merge.CurVal > prev.CurVal };
}

ContractVerifierAllocPass::AllocStatusVal ContractVerifierAllocPass::checkAllocReq(const AllocOperation* AllocOp, Module const& M, const Function* F, std::string& err) {
    const Function* mainF = M.getFunction("main");
    if (!mainF) {
        err = "Cannot find main function, cannot construct path to check precall!";
        return AllocStatusVal::ERROR;
    }
    const Instruction* Entry = &*mainF->getEntryBlock().getFirstNonPHIIt();

    AllocStatus init = { AllocStatusVal::UNDEF, {}};
    IterTypeAlloc data = { {}, {}, AllocOp->contrP, AllocOp->contrParamAccess, F };
    auto bound_transfer = std::bind(&ContractVerifierAllocPass::transferAllocStat, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    auto bound_merge = std::bind(&ContractVerifierAllocPass::mergeAllocStat, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    std::map<const Instruction *, AllocStatus> AnalysisInfo = ContractPassUtility::GenericWorklist<AllocStatus>(Entry, bound_transfer, bound_merge, &data, init);

    //C.DebugInfo->insert(C.DebugInfo->end(), data.dbg.begin(), data.dbg.end());
    //Expr.ErrorInfo->insert(Expr.ErrorInfo->end(), data.err.begin(), data.err.end());

    // Take max over all analysis info
    // Correct usage will not contain error
    AllocStatusVal res = AllocStatusVal::ALLOC;
    for (std::pair<const Instruction*, AllocStatus> AI : AnalysisInfo) {
        if (const CallBase* CB = dyn_cast<CallBase>(AI.first)) {
            if (CB->getCalledFunction() == F) {
                res = std::max(AI.second.CurVal, res);
            }
        }
    }
    return res;
}
