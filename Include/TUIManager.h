
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <json/value.h>

#include "../Passes/ContractManager.hpp"

namespace TUIManager {
    void StartMenu(llvm::ContractManagerAnalysis::ContractDatabase DB);
    void ShowContractDetails(llvm::ContractManagerAnalysis::Contract C);
    void ResultsScreen(Json::Value res);
}
