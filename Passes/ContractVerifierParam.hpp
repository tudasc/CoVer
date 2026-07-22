#pragma once

#include "ContractTree.hpp"
#include <set>

import LLVMModule;
import BasicTypes;

namespace llvm {

class ContractVerifierParamPass : public PassInfoMixin<ContractVerifierParamPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        ModuleAnalysisManager* MAM;
        BasicTypesAnalysis::BasicTypes Basic_Types;
        ContractTree::Fulfillment checkParamReq(std::set<Value*> vars, CallBase* call, int idx, ContractTree::Comparator const& comp, std::string& ErrInfo);
};

} // namespace llvm
