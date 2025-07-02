#include "TUIManager.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <iostream>
#include <string>
#include <vector>

#include "../Passes/ContractManager.hpp"

namespace TUIManager {

ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::FullscreenPrimaryScreen();

int RenderMenu(std::vector<std::string> choices, std::string title) {
    int selected = 0;
    ftxui::MenuOption menu_options = {
        .entries = choices,
        .selected = &selected,
        .on_enter = [&]() { screen.Exit(); }
    };
    ftxui::Component menu = ftxui::Menu(menu_options);
    ftxui::Component render = ftxui::Renderer(
        menu, [&] {
           return ftxui::vbox({
            ftxui::separator(),
            ftxui::text("CoVer: Running in interactive mode") | ftxui::center,
            ftxui::separator(),
            ftxui::text(title) | ftxui::center,
            ftxui::separator(),
            menu->Render(),
            ftxui::separator()
           });
        }
    );
    screen.Loop(render);
    return *menu_options.selected;
}

void StartMenu(llvm::ContractManagerAnalysis::ContractDatabase DB) {
    int choice; // Used for RenderMenu
    choice = RenderMenu({"Start Analysis", "Read Contracts Only"}, "Start Menu");

    if (choice == 1) { // For now, only read contracts
        std::vector<std::string> contr_funcs;
        contr_funcs.push_back("Continue Analysis");
        for (llvm::ContractManagerAnalysis::Contract const& contract : DB.Contracts) {
            contr_funcs.push_back(contract.F->getName().str());
        }
        while (choice != 0) {
            choice = RenderMenu(contr_funcs, "Contract Listing (Read " + std::to_string(DB.Contracts.size()) + " Contracts)");
            if (choice != 0) ShowContractDetails(DB.Contracts[choice-1]);
        }
    }
}

void ShowContractDetails(llvm::ContractManagerAnalysis::Contract C) {
    int selected = 0;
    ftxui::MenuOption menu_options = {
        .entries = std::vector<std::string>{"Back to Listing"},
        .selected = &selected,
        .on_enter = [&]() { screen.Exit(); }
    };
    ftxui::Component menu = ftxui::Menu(menu_options);
    ftxui::Component render = ftxui::Renderer(
        menu, [&] {
           return ftxui::vbox({
            ftxui::separator(),
            ftxui::text("CoVer: Running in interactive mode") | ftxui::center,
            ftxui::separator(),
            ftxui::text("Contract Details: " + C.F->getName().str()) | ftxui::center,
            ftxui::separator(),
            ftxui::text("Full String:"),
            ftxui::text(C.ContractString.str()),
            ftxui::separator(),
            menu->Render(),
            ftxui::separator()
           });
        }
    );
    screen.Loop(render);
}

}
