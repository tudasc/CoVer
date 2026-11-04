module;

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../Passes/ContractManager.hpp"

export module InteractiveUI;
import LLVMModule;
import ContractPassUtility;
import InteractiveSession;
import TUIManager;
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
#ifdef COVER_HAS_GUI
    if (mode == InteractMode::GUI) return GUIManager::StartMenu(DB, parse_time);
#endif
    TUIManager::StartMenu(DB, parse_time);
}

export bool ResultsScreen(std::vector<Contract> const& ViolatedContracts, ContractManagerAnalysis::FormulaReports const& reports, ContractManagerAnalysis::FormulaDebugHandlers& debugHandlers, double runtime) {
#ifdef COVER_HAS_GUI
    if (mode == InteractMode::GUI) return GUIManager::ResultsScreen(ViolatedContracts, reports, debugHandlers, runtime);
#endif
    return TUIManager::ResultsScreen(ViolatedContracts, reports, debugHandlers, runtime);
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
    bool reanalyse = ShowTrace(session);
    if (reanalyse) session.SaveHistory();
    return reanalyse;
}

} // namespace InteractiveUI
