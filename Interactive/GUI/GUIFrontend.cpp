import InteractiveFrontend;
import GUIManager;
import GUITrace;

extern "C" InteractiveFrontend::Frontend const* CoVer_Frontend() {
    static InteractiveFrontend::Frontend const frontend = {
        .isAvailable = GUIManager::IsAvailable,
        .startMCP = GUIManager::StartMCP,
        .startMenu = GUIManager::StartMenu,
        .resultsScreen = GUIManager::ResultsScreen,
        .showTrace = GUITrace::ShowTrace,
    };
    return &frontend;
}
