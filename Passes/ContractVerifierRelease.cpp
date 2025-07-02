#include "ContractVerifierRelease.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "ContractPassUtility.hpp"
#include "ErrorMessage.h"

#include <algorithm>
#include <any>
#include <llvm/Analysis/InlineCost.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;
using namespace ContractTree;

PreservedAnalyses ContractVerifierReleasePass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    ContractManagerAnalysis::ContractDatabase DB = AM.getResult<ContractManagerAnalysis>(M);
    Tags = DB.Tags;

    for (ContractManagerAnalysis::LinearizedContract const& C : DB.LinearizedContracts) {
        for (const std::shared_ptr<ContractExpression> Expr : C.Post) {
            if (*Expr->Status != Fulfillment::UNKNOWN) continue;
            // Contract has a postcondition
            std::string err;
            bool result = false;
            switch (Expr->OP->type()) {
                case OperationType::RELEASE: {
                    const ReleaseOperation& relOP = dynamic_cast<const ReleaseOperation&>(*Expr->OP);
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
    OperationType forbiddenType;
    std::vector<std::any> param;
    std::string releaseFunc;
    std::vector<CallParam> releaseParam;
    const CallBase* callsite;
    std::map<const Function*, std::vector<TagUnit>> Tags;
    const bool isTagRel;
};

void ContractVerifierReleasePass::appendDebugStr(std::vector<ErrorMessage>& err, const Instruction* Forbidden, const CallBase* CB) {
    std::stringstream str;
    std::string type = "operation";

    if (isa<LoadInst>(Forbidden)) type = "load";
    if (isa<StoreInst>(Forbidden)) type = "store";
    if (isa<CallBase>(Forbidden)) type = "call to " + demangle(dyn_cast<CallBase>(Forbidden)->getCalledFunction()->getName());

    str << "Found " << type << " at "
        << ContractPassUtility::getInstrLocStr(Forbidden)
        << " which is in conflict with " << CB->getCalledFunction()->getName().str() << " at " << ContractPassUtility::getInstrLocStr(CB)
        << " before release";

    err.push_back({
        .error_id = "Release",
        .text = str.str(),
        .references = {ContractPassUtility::getErrorReference(Forbidden),
                       ContractPassUtility::getErrorReference(CB),             
        },
    });
}

#define RWHelper(instr) \
    const int forbidParam = std::any_cast<const int>(Data->param[0]); \
    const ParamAccess accType = std::any_cast<const ParamAccess>(Data->param[1]); \
    const Value* contrP = Data->callsite->getArgOperand(forbidParam); \
    if (ContractPassUtility::checkParamMatch(contrP, instr, accType)) { \
        ContractVerifierReleasePass::appendDebugStr(Data->err, instr, Data->callsite); \
        return ContractVerifierReleasePass::ReleaseStatus::ERROR; \
    } \
    return cur;

ContractVerifierReleasePass::ReleaseStatus transferRelease(ContractVerifierReleasePass::ReleaseStatus cur, const Instruction* I, void* data) {
    if (cur == ContractVerifierReleasePass::ReleaseStatus::ERROR) return cur;
    if (cur == ContractVerifierReleasePass::ReleaseStatus::FULFILLED) return cur;

    IterTypeRelease* Data = static_cast<IterTypeRelease*>(data);

    if (const CallBase* CB = dyn_cast<CallBase>(I)) {
        if (ContractPassUtility::checkCalledApplies(CB, Data->releaseFunc, Data->isTagRel, Data->Tags)) {
            if (Data->releaseParam.empty()) return ContractVerifierReleasePass::ReleaseStatus::FULFILLED;
            for (CallParam P : Data->releaseParam) {
                if (ContractPassUtility::checkCallParamApplies(Data->callsite, CB, Data->releaseFunc, P, Data->Tags))
                    return ContractVerifierReleasePass::ReleaseStatus::FULFILLED;
            }
            // Wrong parameters, continue
            return cur;
        }
    }

    switch (Data->forbiddenType) {
        case ContractTree::OperationType::CALL:
        case ContractTree::OperationType::CALLTAG:
            if (const CallBase* CB = dyn_cast<CallBase>(I)) {
                if (ContractPassUtility::checkCalledApplies(CB, std::any_cast<std::string>(Data->param[0]), Data->forbiddenType == ContractTree::OperationType::CALLTAG, Data->Tags)) {
                    // Found forbidden function. Current status is unknown, if we find forbidden parameter (and one is specified) this is an error
                    const std::vector<CallParam> forbidParams = std::any_cast<const std::vector<CallParam>>(Data->param[1]);
                    if (forbidParams.empty()) return ContractVerifierReleasePass::ReleaseStatus::ERROR;
                    for (CallParam forbidParam : forbidParams) {
                        if (ContractPassUtility::checkCallParamApplies(Data->callsite, CB, std::any_cast<std::string>(Data->param[0]), forbidParam, Data->Tags)) {
                            ContractVerifierReleasePass::appendDebugStr(Data->err, CB, Data->callsite);
                            return ContractVerifierReleasePass::ReleaseStatus::ERROR;
                        }
                    }
                    // Did not find relevant parameter
                    return cur;
                }
            }
            break;
        case ContractTree::OperationType::READ:
            if (const LoadInst* LI = dyn_cast<LoadInst>(I)) {
                RWHelper(LI);
            }
            break;
        case ContractTree::OperationType::WRITE:
            if (const StoreInst* SI = dyn_cast<StoreInst>(I)) {
                RWHelper(SI);
            }
            break;
        default:
            llvm_unreachable("transferRelease encountered previously-checked error state!");
    }
    // Not relevant. Just forward info
    return cur;
}

std::pair<ContractVerifierReleasePass::ReleaseStatus,bool> mergeRelease(ContractVerifierReleasePass::ReleaseStatus prev, ContractVerifierReleasePass::ReleaseStatus cur, const Instruction* I, void* data) {
    ContractVerifierReleasePass::ReleaseStatus newStat = std::max(prev, cur);
    if ((prev == ContractVerifierReleasePass::ReleaseStatus::FULFILLED || cur == ContractVerifierReleasePass::ReleaseStatus::FULFILLED) &&
         newStat != ContractVerifierReleasePass::ReleaseStatus::FULFILLED) {
        IterTypeRelease* Data = static_cast<IterTypeRelease*>(data);
        Data->dbg.push_back("[ContractVerifierRelease] NOTE: Successful fulfillment (by release) was lost at " + ContractPassUtility::getInstrLocStr(I) + " due to merging of different branches.");
    }

    // Should continue if newStat is worse than prev
    return { newStat, newStat > prev};
}

ContractVerifierReleasePass::ReleaseStatus ContractVerifierReleasePass::checkRelease(const ContractTree::ReleaseOperation relOp, ContractManagerAnalysis::LinearizedContract const& C, ContractExpression const& Expr, const Module& M, std::string& error) {
    // Figure out release parameters
    OperationType forbiddenType = relOp.Forbidden->type();
    std::vector<std::any> param;
    switch (forbiddenType) {
        case ContractTree::OperationType::CALLTAG:
        case ContractTree::OperationType::CALL:
            param.push_back(dynamic_cast<const CallOperation&>(*relOp.Forbidden).Function);
            param.push_back(dynamic_cast<const CallOperation&>(*relOp.Forbidden).Params);
            break;
        case ContractTree::OperationType::READ:
        case ContractTree::OperationType::WRITE:
            param.push_back(dynamic_cast<const RWOperation&>(*relOp.Forbidden).contrP);
            param.push_back(dynamic_cast<const RWOperation&>(*relOp.Forbidden).contrParamAccess);
            break;
        default:
            error = "Unknown release parameter for forbidden!";
            return ReleaseStatus::ERROR;
    }

    bool isTagRel = false;
    if (relOp.Until->type() == OperationType::CALLTAG) {
        isTagRel = true;
    }
    std::string releaseFunc = dynamic_cast<const CallOperation&>(*relOp.Until).Function;
    std::vector<CallParam> releaseParam = dynamic_cast<const CallOperation&>(*relOp.Until).Params;

    IterTypeRelease data = { {}, {}, forbiddenType, param, releaseFunc, releaseParam, nullptr, Tags, isTagRel};

    // Get all call sites of function, and run analysis
    ReleaseStatus result = ReleaseStatus::FORBIDDEN;
    for (const User* U : C.F->users()) {
        if (const CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledFunction() == C.F) {
                data.callsite = CB;
                std::map<const Instruction *, ReleaseStatus> AnalysisInfo = ContractPassUtility::GenericWorklist<ReleaseStatus>(CB->getNextNode(), transferRelease, mergeRelease, &data, ReleaseStatus::FORBIDDEN);
                C.DebugInfo->insert(C.DebugInfo->end(), data.dbg.begin(), data.dbg.end());
                Expr.ErrorInfo->insert(Expr.ErrorInfo->end(), data.err.begin(), data.err.end());
                data.err.clear();
                for (std::pair<const Instruction *, ReleaseStatus> x : AnalysisInfo) {
                    if (x.second == ReleaseStatus::ERROR) return ReleaseStatus::ERROR;
                }
            }
        }
    }
    return ReleaseStatus::FULFILLED;
}
