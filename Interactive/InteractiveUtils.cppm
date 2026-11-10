module;

#include "ContractTree.hpp"

export module InteractiveUtils;
import std;
import AnalysisReports;
import LLVMModule;
import ErrorMessage;
import ContractPassUtility;
import Worklist;

using namespace ContractTree;

using namespace llvm;

// Frontend independent parts of the interactive mode, shared by the TUI and GUI. Trace commands are in InteractiveCmds.
namespace InteractiveUtils {

using Contract = AnalysisReports::Contract;

export constexpr int UI_ANALYSISINFO_PAD_SIZE = 15;

// Runs a task on the frontend's UI thread, callable from any thread (TUIManager::Post, GUIManager::Post).
// Worker threads may call it after the object they were started by is gone, so it must stay valid for the program's lifetime.
export using PostFn = std::function<void(std::function<void()>)>;

// Files of the interactive mode, in the working directory. The compile wrappers refer to REANALYSE_FILE by name.
export constexpr char const* INTERACT_START_FILE = "CoVer_InteractStart.ll"; // Module before any annotation, base of the diff command
export constexpr char const* REANALYSE_FILE = "CoVer_Reanalyse.ll"; // Current module, input of the reanalysis
export constexpr char const* HISTORY_CACHE_FILE = "CoVer_HistoryCache"; // Command history and MCP mode, carried over to the reanalysis
export constexpr char const* CHAT_CACHE_FILE = "CoVer_ChatCache.json"; // Conversation of the LLM chat, carried over to the reanalysis
export constexpr char const* DIFF_TMP_FILE = "CoVer_Diff.tmp";

export void removeInteractiveFiles() {
    for (char const* file : {INTERACT_START_FILE, REANALYSE_FILE, HISTORY_CACHE_FILE, CHAT_CACHE_FILE, DIFF_TMP_FILE}) std::filesystem::remove(file);
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

export std::string startMenuTitle(std::size_t num_contracts, double parse_time) {
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

// Identifies a trace across analysis runs: one is reported per contract and top level formula, and has a trace per violated leaf.
// Leaf indices may change when a reanalysis fixes other leaves, so the leaf's formula is used.
export std::string TraceKey(std::string const& function, std::string const& report, std::string const& leaf) {
    return function + "|" + report + "|" + leaf;
}

export std::string TraceKey(ReportedFormula const& report, ContractTree::ContractFormula const& leaf) {
    return TraceKey(report.contract->F->getName().str(), report.formula->ExprStr, leaf.ExprStr);
}

export std::string TraceKey(AnalysisReports::DebugContext const& context) {
    return TraceKey(context.function, context.report, context.formula);
}

// Formulas listed on the results screen, all that are not fulfilled
export std::vector<ReportedFormula> getReportedFormulas(std::vector<Contract> const& ViolatedContracts) {
    std::vector<ReportedFormula> formulas;
    for (Contract const& C : ViolatedContracts) {
        for (std::shared_ptr<ContractTree::ContractFormula> const& scope : {C.Data.Pre, C.Data.Post}) {
            if (!scope) continue;
            for (std::shared_ptr<ContractTree::ContractFormula> const& form : scope->Children) {
                if (*form->Status != ContractTree::Fulfillment::FULFILLED) formulas.push_back({form, &C});
            }
        }
    }
    return formulas;
}

// Violated leaves of the formula with a trace that can be debugged, in depth-first order. MCP clients address them by their index.
void CollectLeaves(std::shared_ptr<ContractTree::ContractFormula> const& form, AnalysisReports::FormulaDebugHandlers const& debugHandlers,
                   std::vector<std::shared_ptr<ContractTree::ContractExpression>>& leaves) {
    if (*form->Status != ContractTree::Fulfillment::FULFILLED && debugHandlers.contains(form.get())) leaves.push_back(std::static_pointer_cast<ContractTree::ContractExpression>(form));
    for (std::shared_ptr<ContractTree::ContractFormula> const& child : form->Children) CollectLeaves(child, debugHandlers, leaves);
}

export std::vector<std::shared_ptr<ContractTree::ContractExpression>> DebuggableLeaves(ReportedFormula const& report, AnalysisReports::FormulaDebugHandlers const& debugHandlers) {
    std::vector<std::shared_ptr<ContractTree::ContractExpression>> leaves;
    CollectLeaves(report.formula, debugHandlers, leaves);
    return leaves;
}

export std::string traceKindToStr(Worklist::TraceKind kind) {
    switch (kind) {
        case Worklist::TraceKind::LINEAR: return "LINEAR";
        case Worklist::TraceKind::BRANCH: return "BRANCH";
        case Worklist::TraceKind::SWITCH: return "SWITCH";
        case Worklist::TraceKind::FUNCENTRY: return "FUNCENTRY";
        case Worklist::TraceKind::FUNCEXIT: return "FUNCEXIT";
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

} // namespace InteractiveUtils
