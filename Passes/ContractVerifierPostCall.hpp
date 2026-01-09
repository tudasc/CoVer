#pragma once

#include "ContractPassUtility.hpp"
#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include "ErrorMessage.h"
#include "TUITrace.hpp"
#include "llvm/IR/PassManager.h"
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <map>
#include <set>

namespace llvm {

class ContractVerifierPostCallPass : public PassInfoMixin<ContractVerifierPostCallPass> {
    public:
        enum struct CallStatus { CALLED, NOTCALLED };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static void appendDebugStr(std::string Target, bool isTag, const CallBase* Provider, const std::set<const CallBase *> candidates, std::vector<ErrorMessage>& err, const Instruction* retLoc);

    private:
        CallStatus checkPostCall(const ContractTree::CallOperation* cOP, const ContractManagerAnalysis::LinearizedContract& C, ContractExpression& Expr, const bool isTag, const Module& M, std::string& error);
        std::map<const Function*, std::vector<TagUnit>> Tags;

        static std::string postCallStatusToStr(ContractVerifierPostCallPass::CallStatus S) {
            switch (S) {
                case CallStatus::CALLED: return "CALLED";
                case CallStatus::NOTCALLED: return "NOTCALLED";
            }
        }

        static bool handleDebug(ContractPassUtility::WorklistResult<CallStatus> WLRes, ContractManagerAnalysis::LinearizedContract C) {
            ContractPassUtility::JumpTraceEntry<CallStatus>* startloc = nullptr;
            for (std::pair<const Instruction *, CallStatus> x : WLRes.AnalysisInfo) {
                if (isa<ReturnInst>(x.first) && x.first->getParent()->getParent()->getName() == "main" && x.second == CallStatus::NOTCALLED) {
                    startloc = WLRes.JumpTraces[x.first];
                    break;
                }
            }
            return TUITrace::ShowTrace<CallStatus>(WLRes.JumpTraces, startloc, postCallStatusToStr);
        }
};

} // namespace llvm
