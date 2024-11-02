#include "ContractVerifierRelease.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "ContractPassUtility.hpp"

#include <algorithm>
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
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Transforms/Instrumentation.h>
#include <vector>

using namespace llvm;
using namespace ContractTree;

PreservedAnalyses ContractVerifierReleasePass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    ContractManagerAnalysis::ContractDatabase DB = AM.getResult<ContractManagerAnalysis>(M);

    for (ContractManagerAnalysis::Contract C : DB.Contracts) {
        if (*C.Status == Fulfillment::UNKNOWN && C.Data.Post.has_value()) {
            // Contract has a postcondition
            const ContractExpression& Expr = C.Data.Post.value();
            std::string err;
            bool result = false;
            switch (Expr.OP->type()) {
                case OperationType::RELEASE: {
                    const ReleaseOperation& relOP = dynamic_cast<const ReleaseOperation&>(*Expr.OP);
                    result = checkRelease(relOP, C.F, M, err) == ReleaseStatus::FULFILLED;
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

ContractVerifierReleasePass::ReleaseStatus transferRelease(ContractVerifierReleasePass::ReleaseStatus cur, const Instruction* I, void* data) {
    if (cur == ContractVerifierReleasePass::ReleaseStatus::ERROR) return cur;

    using IterType = struct { OperationType forbiddenType; std::string param; std::string releaseFunc; };
    IterType* Data = static_cast<IterType*>(data);

    if (const CallBase* CB = dyn_cast<CallBase>(I)) {
        if (CB->getCalledFunction()->getName() == Data->releaseFunc) {
            // Found target, and cur is not error. Success for now
            return ContractVerifierReleasePass::ReleaseStatus::FULFILLED;
        }
    }

    switch (Data->forbiddenType) {
        case ContractTree::OperationType::CALL:
            if (const CallBase* CB = dyn_cast<CallBase>(I)) {
                if (CB->getCalledFunction()->getName() == Data->param) {
                    // Found forbidden. If released, ok. Otherwise, RIP
                    if (cur == ContractVerifierReleasePass::ReleaseStatus::FULFILLED) return cur;
                    return ContractVerifierReleasePass::ReleaseStatus::ERROR;
                }
            }
            break;
        case ContractTree::OperationType::READ:
        case ContractTree::OperationType::WRITE:
            #warning TODO RW forbidden for release
            break;
        default:
            llvm_unreachable("transferRelease encountered previously-checked error state!");
    }
    // Not relevant. Just forward info
    return cur;
}

ContractVerifierReleasePass::ReleaseStatus mergeRelease(ContractVerifierReleasePass::ReleaseStatus prev, ContractVerifierReleasePass::ReleaseStatus cur, const Instruction* I, void* data) {
    return std::max(prev, cur);
}

ContractVerifierReleasePass::ReleaseStatus ContractVerifierReleasePass::checkRelease(const ContractTree::ReleaseOperation relOp, const Function* F, const Module& M, std::string& error) {
    // Figure out release parameters
    OperationType forbiddenType = relOp.Forbidden->type();
    std::string param;
    switch (forbiddenType) {
        case ContractTree::OperationType::CALL:
            param = dynamic_cast<const CallOperation&>(*relOp.Forbidden).Function;
            break;
        case ContractTree::OperationType::READ:
            param = dynamic_cast<const ReadOperation&>(*relOp.Forbidden).Variable;
            break;
        case ContractTree::OperationType::WRITE:
            param = dynamic_cast<const WriteOperation&>(*relOp.Forbidden).Variable;
            break;
        default:
            error = "Unknown release parameter for forbidden!";
            return ReleaseStatus::ERROR;
    }
    std::string releaseFunc = dynamic_cast<const CallOperation&>(*relOp.Until).Function;

    using IterType = struct { OperationType forbiddenType; std::string param; std::string releaseFunc; };
    IterType data = { forbiddenType, param, releaseFunc };

    // Get all call sites of function, and run analysis
    ReleaseStatus result = ReleaseStatus::FORBIDDEN;
    for (const User* U : F->users()) {
        if (const CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledFunction() == F) {
                std::map<const Instruction *, ReleaseStatus> AnalysisInfo = GenericWorklist<ReleaseStatus>(CB, transferRelease, mergeRelease, &data, ReleaseStatus::FORBIDDEN);
                for (std::pair<const Instruction *, ReleaseStatus> x : AnalysisInfo) {
                    if (x.second == ReleaseStatus::ERROR) return ReleaseStatus::ERROR;
                }
            }
        }
    }
    return ReleaseStatus::FULFILLED;
}
