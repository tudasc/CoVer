module;

#include <dlfcn.h>

#include "ContractTree.hpp"

export module InteractiveUI;
import std;
import LLVMModule;
import AnalysisReports;
import ContractPassUtility;
import InteractiveSession;
import InteractiveUtils;
import InteractiveFrontend;
import TraceTools;
import MCPControl;
import LLMChat;

using namespace llvm;

// Entry of the analysis into the interactive mode. Loads the library of the selected frontend and dispatches to it.
namespace InteractiveUI {

using InteractiveFrontend::Frontend;

export enum struct InteractMode { NONE, TUI, GUI };

Frontend const* frontend = nullptr;

// The directory of this library, which the frontend libraries are installed next to
std::filesystem::path LibraryDir() {
    Dl_info info;
    if (!dladdr(reinterpret_cast<void const*>(&LibraryDir), &info) || !info.dli_fname) return ".";
    return std::filesystem::path(info.dli_fname).parent_path();
}

// The frontend in the given library, or why it cannot be used
std::expected<Frontend const*, std::string> Load(std::string const& name) {
    std::filesystem::path path = LibraryDir() / ("lib" + name + ".so");
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); // Never closed, the frontend lives until the program exits
    if (!lib) return std::unexpected(dlerror());
    InteractiveFrontend::EntryPoint entry = reinterpret_cast<InteractiveFrontend::EntryPoint>(dlsym(lib, InteractiveFrontend::ENTRY_POINT));
    if (!entry) return std::unexpected(std::format("{} has no entry point {}", path.string(), InteractiveFrontend::ENTRY_POINT));
    Frontend const* loaded = entry();
    if (!loaded->isAvailable()) return std::unexpected("it cannot be shown here (e.g. no display was found)");
    return loaded;
}

// Loads the frontend of the mode. False if it cannot be loaded, then the analysis runs without the interactive mode.
export bool SetMode(InteractMode mode) {
    bool gui = mode == InteractMode::GUI;
    std::expected<Frontend const*, std::string> loaded = Load(gui ? "CoVerQtFrontend" : "CoVerTUIFrontend");
    if (!loaded) {
        errs() << "CoVer: The " << (gui ? "GUI" : "TUI") << " is not available, continuing without the interactive mode: " << loaded.error() << "\n";
        return false;
    }
    frontend = *loaded;
    return true;
}

// The start menu before the analysis, skipped when reanalysing from a trace. Its debug handlers are a copy, so they cannot change the analysis.
// Also starts the MCP server
export void StartMenu(std::vector<AnalysisReports::Contract> const& contracts, AnalysisReports::FormulaReports const& reports, AnalysisReports::FormulaDebugHandlers debugHandlers, double parse_time) {
    if (std::string err = frontend->startMCP(InteractiveSession::ResumeMCPMode()); !err.empty()) errs() << "CoVer: " << err << "\n";
    if (InteractiveSession::Resuming()) return;
    frontend->startMenu(contracts, reports, debugHandlers, parse_time);
}

// Opens the trace debugged before the reanalysis again if it is still reported, returns whether to reanalyse
bool ResumeTrace(std::vector<InteractiveUtils::ReportedFormula> const& formulas, AnalysisReports::FormulaReports const& reports, AnalysisReports::FormulaDebugHandlers& debugHandlers) {
    std::optional<std::string> key = InteractiveSession::TakeResumeTrace();
    if (!key) return false;
    for (InteractiveUtils::ReportedFormula const& report : formulas) {
        for (std::shared_ptr<ContractTree::ContractExpression> const& leaf : InteractiveUtils::DebuggableLeaves(report, debugHandlers)) {
            if (InteractiveUtils::TraceKey(report, *leaf) != *key) continue;
            return debugHandlers[leaf.get()](AnalysisReports::DebugContext(*report.contract, *report.formula, *leaf, reports));
        }
    }
    return false;
}

// The results of the analysis, returns whether to reanalyse. When reanalysing from a trace, it is opened again first.
export bool ResultsScreen(std::vector<AnalysisReports::Contract> const& ViolatedContracts, AnalysisReports::FormulaReports const& reports, AnalysisReports::FormulaDebugHandlers& debugHandlers, double runtime) {
    std::vector<InteractiveUtils::ReportedFormula> formulas = InteractiveUtils::getReportedFormulas(ViolatedContracts);
    MCPControl::ResultsScope mcp_scope(formulas, reports, debugHandlers); // Tool calls of MCP clients work on these reports
    if (ResumeTrace(formulas, reports, debugHandlers)) return true;
    return frontend->resultsScreen(formulas, reports, debugHandlers, runtime);
}

// Shows the trace, returns whether to reanalyse
bool ShowTrace(InteractiveSession::TraceSession& session) {
    TraceTools::TraceTools mcp_tools(session);
    MCPControl::TraceScope mcp_trace(mcp_tools); // Tool calls of MCP clients work on this trace while it is open
    bool reanalyse = frontend->showTrace(session);
    if (reanalyse) {
        session.SaveForReanalysis(MCPControl::Active());
        LLMChat::ExportConversation(InteractiveUtils::CHAT_CACHE_FILE); // The chat continues after the reanalysis
    }
    return reanalyse;
}

export template<typename T>
bool ShowTrace(ContractPassUtility::TraceDB<T> traceDB, ContractPassUtility::JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr, AnalysisReports::DebugContext context) {
    if (!trace) return false;
    InteractiveSession::TraceSessionImpl<T> session(traceDB, trace, infoToStr, std::move(context));
    return ShowTrace(session);
}

} // namespace InteractiveUI
