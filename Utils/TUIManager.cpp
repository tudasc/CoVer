#include "TUIManager.h"

#include <fstream>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <iostream>
#include <string>
#include <vector>

#include "../Passes/ContractManager.hpp"

namespace TUIManager {

constexpr int FILE_CONTEXT_SIZE = 5;

ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::FullscreenPrimaryScreen();
ftxui::Element header = ftxui::vbox({
    ftxui::separator(),
    ftxui::text("CoVer: Running in interactive mode") | ftxui::center,
    ftxui::separator()
});

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
            header,
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
            header,
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

void ShowFile(Json::Value ref) {
    std::string file = ref["file"].asString();
    int line = ref["line"].asInt();
    int column = ref["column"].asInt();
    std::ifstream filestream(file);
    std::string out;

    // First, throw away unneeded lines at the beginning
    int cur_line = 0;
    for (int i = 0; i < std::max(0, line-FILE_CONTEXT_SIZE); i++) {
        std::getline(filestream, out);
        cur_line++;
    }

    // Now, read lines of relevance
    std::vector<ftxui::Element> lines;
    for (int i = 0; i < FILE_CONTEXT_SIZE*2; i++) {
        std::getline(filestream, out);
        cur_line++;
        if (!filestream) break;
        if (cur_line == line) lines.push_back(ftxui::text(out) | ftxui::bgcolor(ftxui::Color::Red));
        else lines.push_back(ftxui::text(out));
    }

    int selected = 0;
    ftxui::MenuOption menu_options = {
        .entries = std::vector<std::string>{"Return to Report Details"},
        .selected = &selected,
        .on_enter = [&]() { screen.Exit(); }
    };
    ftxui::Component menu = ftxui::Menu(menu_options);
    ftxui::Component render = ftxui::Renderer(
        menu, [&] {
           return ftxui::vbox({
            header,
            ftxui::text("File Context") | ftxui::center,
            ftxui::separator(),
            ftxui::vbox(lines),
            ftxui::separator(),
            menu->Render(),
            ftxui::separator()
           });
        }
    );
    screen.Loop(render);
}

void ShowMessageDetails(Json::Value msg) {
    std::vector<std::string> choices;
    choices.push_back("Back to Listing");

    std::vector<ftxui::Element> ref_nodes;
    for (Json::Value ref : msg["references"]) {
        choices.push_back("View Reference: " + ref["file"].asString() + ":" + ref["line"].asString() + ":" + ref["column"].asString());
    }

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
            header,
            ftxui::text("Report Details: " + msg["type"].asString()) | ftxui::center,
            ftxui::separator(),
            ftxui::text("Full Report:"),
            ftxui::text(msg["text"].asString()),
            ftxui::separator(),
            ftxui::vbox(ref_nodes),
            menu->Render(),
            ftxui::separator()
           });
        }
    );
    screen.Loop(render);
    int choice = *menu_options.selected;
    while (choice != 0) {
        ShowFile(msg["references"][choice-1]);
        screen.Loop(render);
        choice = *menu_options.selected;
    }
}

void ResultsScreen(Json::Value res) {
    std::vector<std::string> message_titles;
    message_titles.push_back("Exit Tool");
    for (Json::Value message : res["messages"]) {
        std::string msg_type = message["type"].asString();
        message_titles.push_back(msg_type);
    }
    int choice = 0;
    do {
        choice = RenderMenu(message_titles, "Reported Errors");
        if (choice != 0) ShowMessageDetails(res["messages"][choice-1]);

    } while (choice != 0);
}

}
