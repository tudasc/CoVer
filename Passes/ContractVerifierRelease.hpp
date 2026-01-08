#pragma once

#include "llvm/IR/PassManager.h"
#include <llvm/IR/InstrTypes.h>
#include "ContractPassUtility.hpp"
#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include "ErrorMessage.h"
#include "TUIManager.hpp"

namespace llvm {

class ContractVerifierReleasePass : public PassInfoMixin<ContractVerifierReleasePass> {
    public:
        enum struct ReleaseStatus { FULFILLED, FORBIDDEN, ERROR_UNFULFILLED, ERROR };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

        static void appendDebugStr(std::vector<ErrorMessage>& err, const Instruction* Forbidden, const CallBase* CB);
    private:
        bool printMultiReports = false;
        ReleaseStatus transferRelease(ReleaseStatus cur, const Instruction* I, void* data);
        std::pair<ReleaseStatus,bool> mergeRelease(ReleaseStatus prev, ReleaseStatus cur, const Instruction* I, void* data);
        ReleaseStatus checkRelease(ContractTree::ReleaseOperation const& relOp, ContractManagerAnalysis::LinearizedContract const& C, ContractExpression& Expr, const Module& M, std::string& error);
        std::map<const Function*, std::vector<TagUnit>> Tags;

        static std::string releaseStatusToStr(ReleaseStatus S) {
            switch (S) {
                case ReleaseStatus::FULFILLED: return "FULFILLED";
                case ReleaseStatus::FORBIDDEN: return "FORBIDDEN";
                case ReleaseStatus::ERROR_UNFULFILLED: return "ERROR_UNFULFILLED";
                case ReleaseStatus::ERROR: return "ERROR";
            }
        }

        static bool handleDebug(ContractPassUtility::WorklistResult<ReleaseStatus> WLRes, ContractManagerAnalysis::LinearizedContract C) {
            ContractPassUtility::JumpTraceEntry<ReleaseStatus>* startloc = nullptr;
            for (std::pair<const Instruction*, ReleaseStatus> AI : WLRes.AnalysisInfo) {
                if (AI.second >= ReleaseStatus::ERROR_UNFULFILLED) {
                    // nextnondbg exists, forb will always be mem or call not ret instr
                    startloc = WLRes.JumpTraces[AI.first->getNextNonDebugInstruction()];
                    break;
                }
            }
            return TUIManager::ShowTrace<ReleaseStatus>(WLRes.JumpTraces, startloc, releaseStatusToStr);
        }
};

} // namespace llvm
