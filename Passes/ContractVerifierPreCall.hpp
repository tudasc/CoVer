#pragma once

#include "ContractPassUtility.hpp"
#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include "ErrorMessage.h"
#include "llvm/IR/PassManager.h"
#include <llvm/IR/InstrTypes.h>
#include <map>
#include "TUIManager.hpp"
#include <set>
#include <vector>

namespace llvm {

class ContractVerifierPreCallPass : public PassInfoMixin<ContractVerifierPreCallPass> {
    public:
        enum struct CallStatusVal { CALLED, PARAMCHECK, NOTCALLED, ERROR };
        struct CallStatus {
            CallStatusVal CurVal;
            std::set<const CallBase*> candidate;
        };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static void appendDebugStr(std::string Target, bool isTag, const CallBase* Provider, const std::set<const CallBase *> candidates, std::vector<ErrorMessage>& err);

    private:
        std::pair<CallStatus,bool> mergePreCallStat(CallStatus prev, CallStatus cur, const Instruction* I, void* data);
        CallStatus transferPreCallStat(CallStatus cur, const Instruction* I, void* data);
        CallStatusVal checkPreCall(const ContractTree::CallOperation* cOP, const ContractManagerAnalysis::LinearizedContract& C, ContractExpression& Expr, const bool isTag, const Module& M, std::string& error);
        std::map<const Function*, std::vector<TagUnit>> Tags;

        static std::string preCallStatusToStr(ContractVerifierPreCallPass::CallStatus S) {
            switch (S.CurVal) {
                case CallStatusVal::CALLED: return "CALLED";
                case CallStatusVal::NOTCALLED: return "NOTCALLED";
                case CallStatusVal::PARAMCHECK: return "PARAMCHECK";
                case CallStatusVal::ERROR: return "ERROR";
            }
        }

        static void handleDebug(ContractPassUtility::WorklistResult<CallStatus> WLRes, ContractManagerAnalysis::LinearizedContract C) {
            ContractPassUtility::JumpTraceEntry<CallStatus>* startloc;
            for (std::pair<const Instruction*, CallStatus> AI : WLRes.AnalysisInfo) {
                if (const CallBase* CB = dyn_cast<CallBase>(AI.first)) {
                    if (CB->getCalledFunction() == C.F) {
                        if (AI.second.CurVal == CallStatusVal::ERROR) {
                            // nextnondbg exists, supplier is always a CB so next will at least be block terminator
                            startloc = WLRes.JumpTraces[CB->getNextNonDebugInstruction()];
                            break;
                        }
                    }
                }
            }
            TUIManager::ShowTrace<CallStatus>(WLRes.JumpTraces, startloc, preCallStatusToStr);
        }
};

} // namespace llvm
