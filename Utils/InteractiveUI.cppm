module;

#include <functional>
#include <string>
#include <vector>

#include "../Passes/ContractManager.hpp"

export module InteractiveUI;
import LLVMModule;
import ContractPassUtility;
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

export InteractMode GetMode() { return mode; }

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

export bool ResultsScreen(std::vector<Contract> const& ViolatedContracts, ContractManagerAnalysis::FormulaReports const& reports, double runtime) {
#ifdef COVER_HAS_GUI
    if (mode == InteractMode::GUI) return GUIManager::ResultsScreen(ViolatedContracts, reports, runtime);
#endif
    return TUIManager::ResultsScreen(ViolatedContracts, reports, runtime);
}

export template<typename T>
bool ShowTrace(ContractPassUtility::TraceDB<T> traceDB, ContractPassUtility::JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr) {
#ifdef COVER_HAS_GUI
    if (GetMode() == InteractMode::GUI) return GUITrace::ShowTrace<T>(traceDB, trace, infoToStr);
#endif
    return TUITrace::ShowTrace<T>(traceDB, trace, infoToStr);
}

} // namespace InteractiveUI
