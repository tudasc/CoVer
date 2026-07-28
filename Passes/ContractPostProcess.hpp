#pragma once

#include <memory>
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include <json/json.h>
#include <optional>

import LLVMModule;
import ErrorMessage;

namespace llvm {

class ContractPostProcessingPass : public PassInfoMixin<ContractPostProcessingPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        ContractManagerAnalysis::ContractDatabase* DB;
        int xsucc, xfail, FP, FN, UN;
        void checkExpErr(ContractManagerAnalysis::Contract C);
        Fulfillment checkExpressions(ContractManagerAnalysis::Contract const& C, bool output);
        std::pair<Fulfillment,std::optional<std::string>> resolveFormula(std::shared_ptr<ContractFormula> contrF);
        void outputSubformulaErrs(std::string type, const ContractManagerAnalysis::Contract C, std::map<std::shared_ptr<ContractFormula>, ErrorMessage> reasons);
        raw_ostream& printMsg();

        Json::Value json_messages;
        Json::FastWriter json_writer;

        std::vector<ContractManagerAnalysis::Contract> ViolatedContracts;

        bool isInteractive;
};

} // namespace llvm
