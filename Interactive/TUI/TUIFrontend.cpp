import InteractiveFrontend;
import TUIManager;
import TUITrace;

extern "C" InteractiveFrontend::Frontend const* CoVer_Frontend() {
    static InteractiveFrontend::Frontend const frontend = {
        .isAvailable = [] { return true; },
        .startMCP = TUIManager::ActivateMCP,
        .startMenu = TUIManager::StartMenu,
        .resultsScreen = TUIManager::ResultsScreen,
        .showTrace = TUITrace::ShowTrace,
    };
    return &frontend;
}
