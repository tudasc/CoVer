#pragma once

#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include "llvm/IR/PassManager.h"
#include <llvm/IR/InstrTypes.h>
#include <memory>
#include <map>
#include <set>

using namespace ContractTree;

namespace llvm {

class ContractVerifierAllocPass : public PassInfoMixin<ContractVerifierAllocPass> {
    public:
        enum struct AllocStatusVal { ALLOC, UNDEF, ERROR };
        struct AllocStatus {
            AllocStatusVal CurVal;
            std::set<const Value*> candidate;
        };
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        ContractManagerAnalysis::ContractDatabase* DB;
        ModuleAnalysisManager* MAM;

        std::map<const Function*, std::set<const AllocOperation*>> AllocFuncs;

        AllocStatus transferAllocStat(AllocStatus s, const Instruction* I, void* data);
        std::pair<AllocStatus,bool> mergeAllocStat(AllocStatus prev, AllocStatus cur, const Instruction* I, void* data);

        AllocStatusVal checkAllocReq(const AllocOperation* AllocOp, Module const& M, const Function* F, std::string& err);

};

} // namespace llvm
