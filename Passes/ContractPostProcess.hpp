#pragma once

#include "llvm/IR/PassManager.h"
#include <memory>
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include <json/json.h>

namespace llvm {

class ContractPostProcessingPass : public PassInfoMixin<ContractPostProcessingPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        int xsucc, xfail, FP, FN, UN;
        void checkExpErr(ContractManagerAnalysis::Contract C);
        Fulfillment checkExpressions(ContractManagerAnalysis::Contract const& C, bool output);
        std::pair<Fulfillment,std::optional<ErrorMessage>> resolveFormula(std::shared_ptr<ContractFormula> contrF);
        void outputSubformulaErrs(std::string type, const std::vector<std::shared_ptr<ContractFormula>> set, std::map<std::shared_ptr<ContractFormula>, ErrorMessage> reasons);

        Json::Value json_messages;
        Json::FastWriter json_writer;
};

} // namespace llvm
