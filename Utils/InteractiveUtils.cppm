module;

#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../Passes/ContractManager.hpp"
#include "ContractTree.hpp"

export module InteractiveUtils;
import LLVMModule;
import ErrorMessage;
import ContractPassUtility;

using namespace llvm;

// Frontend independent parts of the interactive mode, shared by the TUI and GUI. Trace commands are in InteractiveCmds.
namespace InteractiveUtils {

using Contract = ContractManagerAnalysis::Contract;

export constexpr int UI_ANALYSISINFO_PAD_SIZE = 15;

// Files of the interactive mode, in the working directory. The compile wrappers refer to REANALYSE_FILE by name.
export constexpr char const* INTERACT_START_FILE = "CoVer_InteractStart.ll"; // Module before any annotation, base of the diff command
export constexpr char const* REANALYSE_FILE = "CoVer_Reanalyse.ll"; // Current module, input of the reanalysis
export constexpr char const* HISTORY_CACHE_FILE = "CoVer_HistoryCache"; // Command history, carried over to the reanalysis
export constexpr char const* DIFF_TMP_FILE = "CoVer_Diff.tmp";

export void removeInteractiveFiles() {
    for (char const* file : {INTERACT_START_FILE, REANALYSE_FILE, HISTORY_CACHE_FILE, DIFF_TMP_FILE}) std::filesystem::remove(file);
}

std::string getSpecificLine(std::string file, int line) {
    std::ifstream filestream(file);
    std::string out;
    for (int i = 0; i < line; i++) {
        std::getline(filestream, out);
    }
    return out;
}

// Duration in seconds, as milliseconds below one second
export std::string formatDuration(double seconds) {
    return seconds < 1 ? std::format("{:.1f}ms", seconds * 1000) : std::format("{:.2f}s", seconds);
}

export std::string startMenuTitle(size_t num_contracts, double parse_time) {
    return std::format("Start Menu ({} contracts to be analysed, parsed in {})", num_contracts, formatDuration(parse_time));
}

export std::string resultsTitle(bool has_reports, double runtime) {
    return std::format("{} (analysed in {})", has_reports ? "Reported Errors" : "No reported errors!", formatDuration(runtime));
}

export struct ReportedFormula {
    std::shared_ptr<ContractTree::ContractFormula> formula;
    Contract const* contract;

    std::string Summary() const { return std::format("{}: {}", contract->F->getName().str(), formula->Message.value_or(formula->ExprStr)); }
};

// Formulas listed on the results screen, all that are not fulfilled
export std::vector<ReportedFormula> getReportedFormulas(std::vector<Contract> const& ViolatedContracts) {
    std::vector<ReportedFormula> formulas;
    for (Contract const& C : ViolatedContracts) {
        for (std::vector<std::shared_ptr<ContractTree::ContractFormula>> const* list : {&C.Data.Pre, &C.Data.Post}) {
            for (std::shared_ptr<ContractTree::ContractFormula> const& form : *list) {
                if (*form->Status != ContractTree::Fulfillment::FULFILLED) formulas.push_back({form, &C});
            }
        }
    }
    return formulas;
}

export std::string traceKindToStr(ContractPassUtility::TraceKind kind) {
    switch (kind) {
        case ContractPassUtility::TraceKind::LINEAR: return "LINEAR";
        case ContractPassUtility::TraceKind::BRANCH: return "BRANCH";
        case ContractPassUtility::TraceKind::SWITCH: return "SWITCH";
        case ContractPassUtility::TraceKind::FUNCENTRY: return "FUNCENTRY";
        case ContractPassUtility::TraceKind::FUNCEXIT: return "FUNCEXIT";
    }
}

// Source code of the instruction's line, empty without debug info
export std::string getSourceCode(Instruction const* I) {
    if (!I->getDebugLoc()) return "";
    FileReference ref = ContractPassUtility::getFileReference(I);
    return getSpecificLine(ref.file, ref.line);
}

export std::string getSourceLine(Instruction const* I, bool translate) {
    if (translate) {
        if (!I->getDebugLoc()) return "";
        FileReference ref = ContractPassUtility::getFileReference(I);
        return std::format("{:>15}:{:0>3} | {}", std::filesystem::path(ref.file).filename().string(), ref.line, getSourceCode(I));
    } else {
        std::string out;
        raw_string_ostream strstream(out);
        I->print(strstream);
        return out;
    }
}

// Leaf formula whose worklist analysis trace can be inspected
export bool isDebuggable(ContractTree::ContractFormula const& form) {
    if (!form.Children.empty()) return false;
    return static_cast<ContractTree::ContractExpression const&>(form).WorklistInfo != nullptr;
}

} // namespace InteractiveUtils
