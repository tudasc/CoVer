module;

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ContractTree.hpp"

export module AnalysisReports;
import LLVMModule;
import ErrorMessage;

// The results of the analysis as the interactive mode sees them: the contracts, the reports of their formulas and how
// to debug them. Produced by ContractManagerAnalysis, which refers to these types under its own name.
namespace AnalysisReports {

using ContractTree::ContractFormula;

export using FormulaReports = std::map<ContractFormula const*, std::vector<ErrorMessage>>;

export struct Contract {
    llvm::Function* F;
    std::string const ContractString;
    ContractTree::ContractData const Data;
    std::shared_ptr<std::vector<std::string>> DebugInfo = std::make_shared<std::vector<std::string>>();
};

// What a trace session debugs
export struct DebugContext {
    DebugContext(Contract const& C, ContractFormula const& top, ContractFormula const& form, FormulaReports const& reports)
        : function{C.F->getName().str()}, contract{C.ContractString}, report{top.ExprStr}, formula{form.ExprStr}, status{ContractTree::FulfillmentStr(*form.Status)},
          errors{reports.contains(&top) ? reports.at(&top) : std::vector<ErrorMessage>()} {}

    std::string function; // Contract supplier
    std::string contract;
    std::string report; // Reported top level formula
    std::string formula; // Formula whose trace is debugged
    std::string status;
    std::vector<ErrorMessage> errors;
};

// Opens the trace debugger for a formula, returns whether to reanalyse
export using FormulaDebugHandlers = std::map<ContractFormula const*, std::function<bool(DebugContext const&)>>;

} // namespace AnalysisReports
