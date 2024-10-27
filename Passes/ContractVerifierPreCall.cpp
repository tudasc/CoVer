#include "ContractVerifierPreCall.hpp"
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
            switch (C.Data.Pre->OP->type()) {
                case OperationType::CALL: {
                    const CallOperation& cOP = dynamic_cast<const CallOperation&>(*Expr.OP);
                    result = checkPreCall(cOP.Function, C.F, M, err) == CallStatus::CALLED;
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

ContractVerifierPreCallPass::CallStatus transferRW(ContractVerifierPreCallPass::CallStatus cur, const Instruction* I, void* data) {
    if (cur == ContractVerifierPreCallPass::CallStatus::ERROR) return cur;

    using IterType = struct { std::string Target; const Function* F; };
    IterType* Data = static_cast<IterType*>(data);
    if (const CallBase* CB = dyn_cast<CallBase>(I)) {
        if (CB->getCalledFunction()->getName() == Data->Target) {
            // Found target, and cur is not error. Success for now
            return ContractVerifierPreCallPass::CallStatus::CALLED;
        }
        if (CB->getCalledFunction() == Data->F) {
            // Found contract supplier. If previously called, safe. If not, RIP
            if (cur == ContractVerifierPreCallPass::CallStatus::CALLED) return cur;
            return ContractVerifierPreCallPass::CallStatus::ERROR;
        }
    }
    // Not a call. Just forward info
    return cur;
}

ContractVerifierPreCallPass::CallStatus mergeRW(ContractVerifierPreCallPass::CallStatus prev, ContractVerifierPreCallPass::CallStatus cur, const Instruction* I, void* data) {
    return std::max(prev, cur);
}

ContractVerifierPreCallPass::CallStatus ContractVerifierPreCallPass::checkPreCall(std::string reqFunc, const Function* F, const Module& M, std::string& error) {
    const Function* mainF = M.getFunction("main");
    if (!mainF) {
        error = "Cannot find main function, cannot construct path to check precall!";
        return CallStatus::NOTCALLED;
    }
    const Instruction* Entry = mainF->getEntryBlock().getFirstNonPHI();

    using IterType = struct { std::string Target; const Function* F; };
    IterType data = { reqFunc, F};
    std::map<const Instruction *, CallStatus> AnalysisInfo = GenericWorklist<CallStatus>(Entry, transferRW, mergeRW, &data, CallStatus::NOTCALLED);

    // Take intersection of all returning instructions
    CallStatus res = CallStatus::CALLED;
    for (const User* U : F->users()) {
        if (const CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledFunction() == F) {
                res = std::max(AnalysisInfo[CB], res);
            }
        }
    }
    return res;
}
