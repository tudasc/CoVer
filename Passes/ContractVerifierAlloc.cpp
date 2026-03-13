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
            switch (Expr->OP->type()) {
                case FormulaType::ALLOC:
                    if (!AllocFuncs.contains(C.F)) AllocFuncs[C.F] = {};
                    AllocFuncs[C.F].insert(static_cast<const AllocOperation*>(Expr->OP.get()));
                    break;
                case FormulaType::FREE:
                    if (!FreeFuncs.contains(C.F)) FreeFuncs[C.F] = {};
                    FreeFuncs[C.F].insert(static_cast<const FreeOperation*>(Expr->OP.get()));
                    break;
                default: continue;
            }
            *Expr->Status = Fulfillment::FULFILLED; // Always fulfilled.
        }
    }

    // Now, do analysis
    for (ContractManagerAnalysis::LinearizedContract const& C : DB->LinearizedContracts) {
        for (const std::shared_ptr<ContractExpression> Expr : C.Pre) {
            if (*Expr->Status != Fulfillment::UNKNOWN) continue;
            // Contract has a precondition
            if (Expr->OP->type() != FormulaType::ALLOC) continue;
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
    if (StoreInst const* SI = dyn_cast<StoreInst>(I)) {
        if (cur.hasAllocInfo(SI->getValueOperand())) {
            cur.addCopy(SI->getPointerOperand(), SI->getValueOperand(), I->getModule()->getFunction("_QQmain") ? ParamAccess::NORMAL : ParamAccess::ADDROF);
        }
    }

    // Propagate allocations - Pretty much just Fortran from here...
    if (CallBase const* CB = dyn_cast<CallBase>(I)) {
        if (CB->getCalledFunction() && CB->getCalledFunction()->getName().starts_with("llvm.memcpy.p0.p0")) {
            Value* src = CB->getArgOperand(1);
            Value* dest = CB->getArgOperand(0);
            if (ContractPassUtility::isTrivialAlloc(src)) {
                cur.addAllocatedValue(dest);
            }
        }
    }
    if (LoadInst const* LI = dyn_cast<LoadInst>(I)) {
        if (ContractPassUtility::isTrivialAlloc(LI->getPointerOperand())) {
            cur.addAllocatedValue(LI);
        } else if (cur.hasAllocInfo(LI->getPointerOperand())) {
            cur.addCopy(LI, LI->getPointerOperand(), cur.getAllocInfo(LI->getPointerOperand()).acc);
        }
    }
    if (GetElementPtrInst const* GEP = dyn_cast<GetElementPtrInst>(I)) {
        if (cur.hasAllocInfo(GEP->getPointerOperand())) {
            cur.addCopy(GEP, GEP->getPointerOperand(), cur.getAllocInfo(GEP->getPointerOperand()).acc);
        }
    }
    if (InsertValueInst const* IVI = dyn_cast<InsertValueInst>(I)) {
        if (cur.hasAllocInfo(IVI->getInsertedValueOperand())) {
            cur.addCopy(IVI, IVI->getInsertedValueOperand(), cur.getAllocInfo(IVI->getInsertedValueOperand()).acc);
        } else if (ContractPassUtility::isTrivialAlloc(IVI->getInsertedValueOperand())) {
            cur.addAllocatedValue(IVI);
        }
    }
    if (ExtractValueInst const* EVI = dyn_cast<ExtractValueInst>(I)) {
        if (cur.hasAllocInfo(EVI->getAggregateOperand())) {
            cur.addCopy(EVI, EVI->getAggregateOperand(), cur.getAllocInfo(EVI->getAggregateOperand()).acc);
        }
    }
    if (IntToPtrInst const* ITPI = dyn_cast<IntToPtrInst>(I)) {
        if (cur.hasAllocInfo(ITPI->getOperand(0))) {
            cur.addCopy(ITPI, ITPI->getOperand(0), cur.getAllocInfo(ITPI->getOperand(0)).acc);
        }
    }

    if (const CallBase* CB = dyn_cast<CallBase>(I)) {
        if (AllocFuncs.contains(CB->getCalledOperand())) {
            for (const AllocOperation* alloc : AllocFuncs[CB->getCalledOperand()]) {
                #warning TODO different access patterns
                if (alloc->contrP == 99) cur.addAllocatedValue(CB, alloc->contrParamAccess);
                else cur.addAllocatedValue(CB->getArgOperand(alloc->contrP), alloc->contrParamAccess);
            }
            // Dont return here! Maybe it also is contr sup
        }
        if (FreeFuncs.contains(CB->getCalledFunction())) {
            for (const FreeOperation* freeOp : FreeFuncs[CB->getCalledFunction()]) {
                #warning TODO perform free, remove stuff from candidate tree
                cur.freeValue(CB->getArgOperand(freeOp->contrP));
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
            for (std::pair<Value const*, AllocStatus::AllocInfo> Candidate : cur.candidates()) {
                if (ContractPassUtility::checkParamMatch(CB->getArgOperand(Data->param), Candidate.first, Candidate.second.acc, MAM)) {
                    // Success!
                    cur.CurVal = AllocStatusVal::ALLOC;
                    return cur;
                }
            }
            // Any required parameter not used by any candidate
            cur.CurVal = AllocStatusVal::ERROR;
            return cur;
        }
    }
    // Not a call. Just forward info
    return cur;
}

std::pair<ContractVerifierAllocPass::AllocStatus,bool> ContractVerifierAllocPass::mergeAllocStat(AllocStatus prev, AllocStatus cur, const Instruction* I, void* data) {
    AllocStatus intersect = cur.intersect(prev);
    return {intersect, intersect.CurVal > prev.CurVal};
}

ContractVerifierAllocPass::AllocStatusVal ContractVerifierAllocPass::checkAllocReq(const AllocOperation* AllocOp, Module const& M, const Function* F, std::string& err) {
    const Function* mainF = M.getFunction("main");
    if (!mainF) {
        err = "Cannot find main function, cannot construct path to check precall!";
        return AllocStatusVal::ERROR;
    }
    const Instruction* Entry = &*mainF->getEntryBlock().getFirstNonPHIIt();

    AllocStatus init;
    IterTypeAlloc data = { {}, {}, AllocOp->contrP, AllocOp->contrParamAccess, F };
    auto bound_transfer = std::bind(&ContractVerifierAllocPass::transferAllocStat, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    auto bound_merge = std::bind(&ContractVerifierAllocPass::mergeAllocStat, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    std::map<const Instruction *, AllocStatus> AnalysisInfo = ContractPassUtility::GenericWorklist<AllocStatus>(Entry, bound_transfer, bound_merge, &data, init);

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
