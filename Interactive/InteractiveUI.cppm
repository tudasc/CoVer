module;

#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../Passes/ContractManager.hpp"

export module InteractiveUI;
import LLVMModule;
import ContractPassUtility;
import InteractiveSession;
import InteractiveUtils;
import TraceTools;
import MCPControl;
import TUIManager;
import TUIMCP;
import TUITrace;
#ifdef COVER_HAS_GUI
import GUIManager;
import GUITrace;
#endif

using namespace llvm;

// Dispatches the interactive mode to the selected frontend
namespace InteractiveUI {

using Contract = llvm::ContractManagerAnalysis::Contract;

export enum struct InteractMode { NONE, TUI, GUI };

InteractMode mode = InteractMode::NONE;

export void SetMode(InteractMode m) {
#ifdef COVER_HAS_GUI
    if (m == InteractMode::GUI && !GUIManager::IsAvailable()) {
        errs() << "CoVer: No display found for the GUI, falling back to the TUI\n";
        m = InteractMode::TUI;
    }
#else
    if (m == InteractMode::GUI) {
        errs() << "CoVer: Built without GUI support, falling back to the TUI\n";
        m = InteractMode::TUI;
    }
#endif
    mode = m;
}

export void StartMenu(llvm::ContractManagerAnalysis::ContractDatabase DB, double parse_time) {
    // MCP was active before the reanalysis, so MCP clients continue without interaction
    if (MCPControl::CheckResume()) {
        std::string err;
#ifdef COVER_HAS_GUI
        if (mode == InteractMode::GUI) err = GUIManager::StartMCP();
        else
#endif
        err = TUIMCP::Activate();
        if (err.empty()) return; // Skips the start menu
        errs() << "CoVer: " << err << "\n";
    }
#ifdef COVER_HAS_GUI
    if (mode == InteractMode::GUI) return GUIManager::StartMenu(DB, parse_time);
#endif
    TUIManager::StartMenu(DB, parse_time);
}

export bool ResultsScreen(std::vector<Contract> const& ViolatedContracts, ContractManagerAnalysis::FormulaReports const& reports, ContractManagerAnalysis::FormulaDebugHandlers& debugHandlers, double runtime) {
    std::vector<InteractiveUtils::ReportedFormula> formulas = InteractiveUtils::getReportedFormulas(ViolatedContracts);
    MCPControl::ResultsScope mcp_scope(formulas, reports, debugHandlers); // Tool calls of MCP clients work on these reports
#ifdef COVER_HAS_GUI
    if (mode == InteractMode::GUI) return GUIManager::ResultsScreen(formulas, reports, debugHandlers, runtime);
#endif
    while (true) {
        MCPControl::Action action = MCPControl::PendingAction();
        if (action.kind == MCPControl::Action::Kind::OPEN_REPORT) {
            if (MCPControl::OpenPendingReport()) return true;
            continue;
        }
        if (MCPControl::Active()) {
            TUIMCP::Show(std::format("Results of the analysis: {} reports, no trace open", formulas.size()));
            continue;
        }
        switch (TUIManager::ResultsScreen(formulas, reports, debugHandlers, runtime)) {
            case TUIManager::ResultsChoice::EXIT: return false;
            case TUIManager::ResultsChoice::REANALYSE: return true;
            case TUIManager::ResultsChoice::CONTINUE: break;
            case TUIManager::ResultsChoice::MCP:
                if (std::string err = TUIMCP::Activate(); !err.empty()) TUIManager::ShowMessage("MCP Mode", err);
                break;
        }
    }
}

bool ShowTrace(InteractiveSession::TraceSession& session) {
#ifdef COVER_HAS_GUI
    if (mode == InteractMode::GUI) return GUITrace::ShowTrace(session);
#endif
    return TUITrace::ShowTrace(session);
}

export template<typename T>
bool ShowTrace(ContractPassUtility::TraceDB<T> traceDB, ContractPassUtility::JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr, ContractManagerAnalysis::DebugContext context) {
    if (!trace) return false;
    InteractiveSession::TraceSessionImpl<T> session(traceDB, trace, infoToStr, std::move(context));
    TraceTools::TraceTools mcp_tools(session);
    MCPControl::TraceScope mcp_trace(mcp_tools); // Tool calls of MCP clients work on this trace while it is open
    bool reanalyse = ShowTrace(session);
    if (reanalyse) {
        session.SaveHistory();
        MCPControl::SaveResume(session.Context()); // MCP mode continues after the reanalysis
    }
    return reanalyse;
}

} // namespace InteractiveUI
