#pragma once

#include "ContractTree.hpp"
#include "ContractManager.hpp"

import LLVMModule;
import ContractPassUtility;
import InteractiveUI;
import ErrorMessage;

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
        ContractManagerAnalysis::ContractDatabase* DB;
        ModuleAnalysisManager* MAM;

        static std::string releaseStatusToStr(ReleaseStatus S) {
            switch (S) {
                case ReleaseStatus::FULFILLED: return "FULFILLED";
                case ReleaseStatus::FORBIDDEN: return "FORBIDDEN";
                case ReleaseStatus::ERROR_UNFULFILLED: return "ERROR_UNFULFILLED";
                case ReleaseStatus::ERROR: return "ERROR";
            }
        }

        static bool handleDebug(ContractPassUtility::WorklistResult<ReleaseStatus> WLRes, Instruction* Forbidden) {
            // Start trace right after the first forbidden operation found
            return InteractiveUI::ShowTrace<ReleaseStatus>(WLRes.JumpTraces, ContractPassUtility::getTraceAfter(WLRes.JumpTraces, Forbidden), releaseStatusToStr);
        }
};

} // namespace llvm
