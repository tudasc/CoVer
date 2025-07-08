#include "TUIManager.hpp"

#include <fstream>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <iostream>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <vector>

#include "../Passes/ContractManager.hpp"
#include "ContractTree.hpp"

namespace TUIManager {

ftxui::Decorator FulfillmentColor(Fulfillment f) {
    switch (f) {
        case Fulfillment::FULFILLED: return ftxui::bgcolor(ftxui::Color::Green);
        case Fulfillment::UNKNOWN: return ftxui::bgcolor(ftxui::Color::Yellow);
        case Fulfillment::BROKEN: return ftxui::bgcolor(ftxui::Color::Red);

    }
}

constexpr int FILE_CONTEXT_SIZE = 5;

std::string traceKindToStr(ContractPassUtility::TraceKind kind) {
    switch (kind) {
        case ContractPassUtility::TraceKind::LINEAR: return "LINEAR";
        case ContractPassUtility::TraceKind::BRANCH: return "BRANCH";
        case ContractPassUtility::TraceKind::FUNCENTRY: return "FUNCENTRY";
        case ContractPassUtility::TraceKind::FUNCEXIT: return "FUNCEXIT";
    }
}

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
            ftxui::yframe(menu->Render()),
            ftxui::separator()
           });
        }
    );
    screen.Loop(render);
    return *menu_options.selected;
}

std::string RenderTxtEntry(std::vector<std::string> lines, std::string title, std::string last_res) {
    std::string input_str;
    ftxui::InputOption input_options = {
        .content = &input_str,
        .placeholder = "Type \"help\" for help",
        .multiline = false,
        .on_enter = [&]() { screen.Exit(); }
    };
    ftxui::Component input_comp = ftxui::Input(input_options);
    std::vector<ftxui::Element> full_lines;
    for (std::string line : lines) {
        full_lines.push_back(ftxui::xframe(ftxui::text(line)));
    }
    ftxui::Component render = ftxui::Renderer(
        input_comp, [&] {
           return ftxui::vbox({
            header,
            ftxui::text(title) | ftxui::center,
            ftxui::separator(),
            ftxui::yframe(ftxui::vbox(full_lines)),
            ftxui::separator(),
            ftxui::text(last_res),
            ftxui::hbox(ftxui::text(">>> "), input_comp->Render())
           });
        }
    );
    screen.Loop(render);
    return *input_options.content;
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

void ShowContractFormula(std::shared_ptr<ContractTree::ContractFormula> Form, std::string title) {
    int selected = 0;
    std::vector<std::string> menu_entries = {"Exit debugger"};
    if (Form->Children.empty() && std::dynamic_pointer_cast<ContractExpression>(Form)->WorklistInfo) menu_entries.push_back("Inspect formula worklist analysis");
    for (std::shared_ptr<ContractFormula> subform : Form->Children) {
        menu_entries.push_back("Inspect " + FulfillmentStr(*subform->Status) + " subformula: " + Form->ExprStr);
    }
    ftxui::MenuOption menu_options = {
        .entries = menu_entries,
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
            ftxui::hbox(ftxui::text("Contract Formula: " + Form->ExprStr)),
            ftxui::hbox(ftxui::text("Status: "), ftxui::text(FulfillmentStr(*Form->Status)) | FulfillmentColor(*Form->Status)),
            ftxui::separator(),
            menu->Render(),
            ftxui::separator()
           });
        }
    );
    screen.Loop(render);
    if (selected == 0) return;
    if (Form->Children.empty() && std::dynamic_pointer_cast<ContractExpression>(Form)->WorklistInfo) {
        std::shared_ptr<ContractExpression> Expr = std::dynamic_pointer_cast<ContractExpression>(Form);
        Expr->WorklistInfo->handleDebug();
    } else {
        ShowContractFormula(Form->Children[selected], title);
    }
}

void ShowContractDetails(ContractManagerAnalysis::Contract C) {
    std::vector<std::string> menu_entries = {"Back to Listing"};
    std::vector<std::shared_ptr<ContractFormula>> violated_formulas;
    for (int i = 0; i < C.Data.Pre.size(); i++) {
        if (*C.Data.Pre[i]->Status != Fulfillment::BROKEN) continue;
        menu_entries.push_back("Inspect " + FulfillmentStr(*C.Data.Pre[i]->Status) + " Precondition Subformula: " + C.Data.Pre[i]->ExprStr);
        violated_formulas.push_back(C.Data.Pre[i]);
    }
    for (int i = 0; i < C.Data.Post.size(); i++) {
        if (*C.Data.Post[i]->Status != Fulfillment::BROKEN) continue;
        menu_entries.push_back("Inspect Postcondition Subformula: " + C.Data.Post[i]->ExprStr);
        violated_formulas.push_back(C.Data.Post[i]);
    }
    int selected = 0;
    ftxui::MenuOption menu_options = {
        .entries = menu_entries,
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
    if (*menu_options.selected == 0) return;
    ShowContractFormula(violated_formulas[*menu_options.selected-1], "Inspecting Contract Subformula for Function: " + C.F->getName().str());
}

void ShowFile(Json::Value ref) {
    std::string file = ref["file"].asString();
    int line = ref["line"].asInt();
    int column = ref["column"].asInt();
    std::ifstream filestream(file);
    std::string out;

    // Now, read lines of relevance
    int cur_line = 0;
    std::vector<ftxui::Element> lines;
    while (filestream) {
        std::getline(filestream, out);
        cur_line++;
        if (cur_line == line) lines.push_back(ftxui::text(out) | ftxui::bgcolor(ftxui::Color::Red) | ftxui::focus);
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
            ftxui::yframe(ftxui::vbox(lines)) | ftxui::size(ftxui::HEIGHT, ftxui::Constraint::LESS_THAN, 10),
            ftxui::separator(),
            menu->Render(),
            ftxui::separator()
           });
        }
    );
    screen.Loop(render);
}

bool ShowMessageDetails(Json::Value msg) {
    std::vector<std::string> choices;
    choices.push_back("Back to Listing");
    choices.push_back("Launch Debugger");

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
    while (choice > 1) { // File Reference Inspection
        ShowFile(msg["references"][choice-2]);
        screen.Loop(render);
        choice = *menu_options.selected;
    }
    return choice == 1; // Debug or no debug
}

void ResultsScreen(Json::Value res, std::map<Json::Value, const Contract> JsonMsgToContr) {
    std::vector<std::string> message_titles;
    message_titles.push_back("Exit Tool");
    for (Json::Value message : res["messages"]) {
        std::string msg_type = message["type"].asString();
        message_titles.push_back(msg_type);
    }
    int choice = 0;
    do {
        choice = RenderMenu(message_titles, "Reported Errors");
        if (choice != 0) {
            bool debug = ShowMessageDetails(res["messages"][choice-1]);
            if (debug) {
                Contract C = JsonMsgToContr[res["messages"][choice-1]];
                ShowContractDetails(C);
            }
        }
    } while (choice != 0);
}

}
