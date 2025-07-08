#pragma once

#include "llvm/IR/PassManager.h"
#include <llvm/Support/WithColor.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include <json/json.h>
#include <optional>

namespace llvm {

class ContractPostProcessingPass : public PassInfoMixin<ContractPostProcessingPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        int xsucc, xfail, FP, FN, UN;
        void checkExpErr(ContractManagerAnalysis::Contract C);
        Fulfillment checkExpressions(ContractManagerAnalysis::Contract const& C, bool output);
        std::pair<Fulfillment,std::optional<ErrorMessage>> resolveFormula(std::shared_ptr<ContractFormula> contrF);
        void outputSubformulaErrs(std::string type, const ContractManagerAnalysis::Contract C, std::map<std::shared_ptr<ContractFormula>, ErrorMessage> reasons);
        raw_ostream& printMsg();

        Json::Value json_messages;
        Json::FastWriter json_writer;

        std::map<Json::Value, const ContractManagerAnalysis::Contract> JsonMsgToContr;

        bool isInteractive;
};

} // namespace llvm
