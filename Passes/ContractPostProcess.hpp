#pragma once

#include "llvm/IR/PassManager.h"
#include <memory>
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "json.hpp"

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

        nlohmann::ordered_json json_messages;
};

} // namespace llvm
