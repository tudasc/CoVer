#include "ContractVerifierRelease.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"

import std;
import LLVMModule;
import ContractPassUtility;
import Worklist;
import InteractiveUI;
import ErrorMessage;

using namespace llvm;
using namespace ContractTree;

namespace {
    std::string releaseStatusToStr(ContractVerifierReleasePass::ReleaseStatus S) {
        switch (S) {
            case ContractVerifierReleasePass::ReleaseStatus::FULFILLED: return "FULFILLED";
            case ContractVerifierReleasePass::ReleaseStatus::FORBIDDEN: return "FORBIDDEN";
            case ContractVerifierReleasePass::ReleaseStatus::ERROR_UNFULFILLED: return "ERROR_UNFULFILLED";
            case ContractVerifierReleasePass::ReleaseStatus::ERROR: return "ERROR";
        }
    }

    bool handleDebug(Worklist::WorklistResult<ContractVerifierReleasePass::ReleaseStatus> WLRes, Instruction* Forbidden, ContractManagerAnalysis::DebugContext const& context) {
        // Start trace right after the first forbidden operation found
        return InteractiveUI::ShowTrace<ContractVerifierReleasePass::ReleaseStatus>(WLRes.JumpTraces, Worklist::getTraceAfter(WLRes.JumpTraces, Forbidden), releaseStatusToStr, context);
    }
}

PreservedAnalyses ContractVerifierReleasePass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    DB = &AM.getResult<ContractManagerAnalysis>(M);
    MAM = &AM;
    printMultiReports = DB->allowMultiReports;

    for (ContractManagerAnalysis::LinearizedContract const& C : DB->LinearizedContracts) {
        for (std::shared_ptr<ContractExpression> const& Expr : C.Post) {
            if (*Expr->Status != Fulfillment::UNKNOWN) continue;
            // Contract has a postcondition
            std::string err;
            bool result = false;
            switch (Expr->OP->type()) {
                case FormulaType::RELEASE: {
                    ReleaseOperation const& relOP = static_cast<const ReleaseOperation&>(*Expr->OP);
                    C.DebugInfo->push_back("[ContractVerifierRelease] Attempting to verify expression: " + Expr->ExprStr);
                    result = checkRelease(relOP, C, *Expr, M, err) == ReleaseStatus::FULFILLED;
                    break;
                }
                default: continue;
            }
            if (!err.empty()) {
                errs() << err << "\n";
            }
            if (result) {
                *Expr->Status = Fulfillment::FULFILLED;
            } else {
                *Expr->Status = Fulfillment::BROKEN;
            }
        }
    }

    return PreservedAnalyses::all();
}

struct IterTypeRelease {
    std::vector<ErrorMessage> err;
    std::vector<std::string> dbg;
    FormulaType forbiddenType;
    std::vector<std::any> param;
    std::string releaseFunc;
    std::vector<CallParam> releaseParam;
    const CallBase* callsite;
    std::map<Function*, std::vector<TagUnit>> Tags;
    const bool isTagRel;
    const Instruction* forbidden = nullptr; // First forbidden operation found for current callsite
};

static void reportForbidden(IterTypeRelease* Data, const Instruction* Forbidden) {
    ContractVerifierReleasePass::appendDebugStr(Data->err, Forbidden, Data->callsite);
    if (!Data->forbidden) Data->forbidden = Forbidden;
}

void ContractVerifierReleasePass::appendDebugStr(std::vector<ErrorMessage>& err, const Instruction* Forbidden, const CallBase* CB) {
    std::stringstream str;
    std::string type = "operation";

    if (isa<LoadInst>(Forbidden)) type = "load";
    if (isa<StoreInst>(Forbidden)) type = "store";
    if (isa<CallBase>(Forbidden)) type = "call to " + demangle(dyn_cast<CallBase>(Forbidden)->getCalledOperand()->getName());

    str << "Found " << type << " at "
        << ContractPassUtility::getInstrLocStr(Forbidden)
        << " which is in conflict with " << CB->getCalledOperand()->getName().str() << " at " << ContractPassUtility::getInstrLocStr(CB)
        << " before release";

    err.push_back({
        .error_id = "Release",
        .text = str.str(),
        .references = {ContractPassUtility::getFileReference(Forbidden),
                       ContractPassUtility::getFileReference(CB),             
        },
    });
}

#define RWHelper(instr) \
    const int forbidParam = std::any_cast<const int>(Data->param[0]); \
    const ParamAccess accType = std::any_cast<const ParamAccess>(Data->param[1]); \
    const Value* contrP = Data->callsite->getArgOperand(forbidParam); \
    if (ContractPassUtility::checkParamMatch(contrP, instr, accType, MAM)) { \
        reportForbidden(Data, instr); \
        return ReleaseStatus::ERROR_UNFULFILLED; \
    } \
    return cur;

ContractVerifierReleasePass::ReleaseStatus ContractVerifierReleasePass::transferRelease(ReleaseStatus cur, const Instruction* I, void* data) {
    if (cur == ReleaseStatus::ERROR || (!printMultiReports && cur == ReleaseStatus::ERROR_UNFULFILLED)) return cur;
    if (cur == ReleaseStatus::FULFILLED) return cur;

    IterTypeRelease* Data = static_cast<IterTypeRelease*>(data);

    if (const CallBase* CB = dyn_cast<CallBase>(I)) {
        if (ContractPassUtility::checkCalledApplies(CB, Data->releaseFunc, Data->isTagRel, Data->Tags)) {
            if (Data->releaseParam.empty()) return ReleaseStatus::FULFILLED;
            for (CallParam const& P : Data->releaseParam) {
                if (ContractPassUtility::checkCallParamApplies(Data->callsite, CB, Data->releaseFunc, P, Data->Tags, MAM))
                    return cur == ReleaseStatus::ERROR_UNFULFILLED ? ReleaseStatus::ERROR : ReleaseStatus::FULFILLED;
            }
            // Wrong parameters, continue
            return cur;
        }
    }

    switch (Data->forbiddenType) {
        case ContractTree::FormulaType::CALL:
        case ContractTree::FormulaType::CALLTAG:
            if (const CallBase* CB = dyn_cast<CallBase>(I)) {
                if (ContractPassUtility::checkCalledApplies(CB, std::any_cast<std::string>(Data->param[0]), Data->forbiddenType == ContractTree::FormulaType::CALLTAG, Data->Tags)) {
                    // Found forbidden function. Current status is unknown, if we find forbidden parameter (and one is specified) this is an error
                    std::vector<CallParam> const& forbidParams = *std::any_cast<std::vector<CallParam>>(&Data->param[1]);
                    if (forbidParams.empty()) {
                        reportForbidden(Data, CB);
                        return ReleaseStatus::ERROR_UNFULFILLED;
                    }
                    for (CallParam const& forbidParam : forbidParams) {
                        if (ContractPassUtility::checkCallParamApplies(Data->callsite, CB, std::any_cast<std::string>(Data->param[0]), forbidParam, Data->Tags, MAM)) {
                            reportForbidden(Data, CB);
                            return ReleaseStatus::ERROR_UNFULFILLED;
                        }
                    }
                    // Did not find relevant parameter
                    return cur;
                }
            }
            break;
        case ContractTree::FormulaType::READ:
            if (const LoadInst* LI = dyn_cast<LoadInst>(I)) {
                RWHelper(LI);
            }
            break;
        case ContractTree::FormulaType::WRITE:
            if (const StoreInst* SI = dyn_cast<StoreInst>(I)) {
                RWHelper(SI);
            }
            break;
        default:
            std::unreachable(); // transferRelease encountered previously-checked error state!
    }
    // Not relevant. Just forward info
    return cur;
}

std::pair<ContractVerifierReleasePass::ReleaseStatus,bool> ContractVerifierReleasePass::mergeRelease(ReleaseStatus prev, ReleaseStatus cur, const Instruction* I, void* data) {
    ReleaseStatus newStat = std::max(prev, cur);
    return { newStat, newStat > cur};
}

ContractVerifierReleasePass::ReleaseStatus ContractVerifierReleasePass::checkRelease(ContractTree::ReleaseOperation const& relOp, ContractManagerAnalysis::LinearizedContract const& C, ContractExpression& Expr, const Module& M, std::string& error) {
    // Figure out release parameters
    FormulaType forbiddenType = relOp.Forbidden->type();
    std::vector<std::any> param;
    switch (forbiddenType) {
        case ContractTree::FormulaType::CALLTAG:
        case ContractTree::FormulaType::CALL:
            param.push_back(static_cast<const CallOperation&>(*relOp.Forbidden).Function);
            param.push_back(static_cast<const CallOperation&>(*relOp.Forbidden).Params);
            break;
        case ContractTree::FormulaType::READ:
        case ContractTree::FormulaType::WRITE:
            param.push_back(static_cast<const RWOperation&>(*relOp.Forbidden).contrP);
            param.push_back(static_cast<const RWOperation&>(*relOp.Forbidden).contrParamAccess);
            break;
        default:
            error = "Unknown release parameter for forbidden!";
            return ReleaseStatus::ERROR;
    }

    bool isTagRel = false;
    if (relOp.Until->type() == FormulaType::CALLTAG) {
        isTagRel = true;
    }
    std::string releaseFunc = static_cast<const CallOperation&>(*relOp.Until).Function;
    std::vector<CallParam> releaseParam = static_cast<const CallOperation&>(*relOp.Until).Params;

    IterTypeRelease data = { {}, {}, forbiddenType, param, releaseFunc, releaseParam, nullptr, DB->Tags, isTagRel};
    Worklist::TransferFunction<ReleaseStatus> transfer = std::bind(&ContractVerifierReleasePass::transferRelease, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    Worklist::MergeFunction<ReleaseStatus> merge = std::bind(&ContractVerifierReleasePass::mergeRelease, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);

    // Get all call sites of function, and run analysis
    ReleaseStatus result = ReleaseStatus::FORBIDDEN;
    for (User* U : C.F->users()) {
        if (CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledOperand() == C.F) {
                data.callsite = CB;
                Worklist::WorklistResult<ReleaseStatus> WLRes = Worklist::GenericWorklist<ReleaseStatus>(CB, false, transfer, merge, &data, ReleaseStatus::FORBIDDEN);
                C.DebugInfo->insert(C.DebugInfo->end(), data.dbg.begin(), data.dbg.end());
                data.dbg.clear();
                DB->reports[&Expr].insert(DB->reports[&Expr].end(), data.err.begin(), data.err.end());
                data.err.clear();
                // Error states only arise from a forbidden operation
                if (data.forbidden) {
                    DB->debugHandlers[&Expr] = std::bind(&handleDebug, WLRes, const_cast<Instruction*>(data.forbidden), std::placeholders::_1);
                    return ReleaseStatus::ERROR;
                }
            }
        }
    }
    return ReleaseStatus::FULFILLED;
}
