#include "TUIManager.hpp"

#include <fstream>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
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

std::string RenderTxtEntry(std::vector<ftxui::Element> lines, std::string title, std::string last_res) {
    std::string input_str;
    ftxui::InputOption input_options = {
        .content = &input_str,
        .placeholder = "Type \"help\" for help",
        .multiline = false,
        .on_enter = [&]() { screen.Exit(); }
    };
    ftxui::Component input_comp = ftxui::Input(input_options);
    std::vector<ftxui::Element> full_lines;
    for (ftxui::Element line : lines) {
        full_lines.push_back(ftxui::xframe(line));
    }
    ftxui::Component render = ftxui::Renderer(
        input_comp, [&] {
           return ftxui::vbox({
            header,
            ftxui::text(title) | ftxui::center,
            ftxui::separator(),
            ftxui::yframe(ftxui::vbox(full_lines)),
            ftxui::separator(),
            ftxui::filler(),
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

void ShowFile(std::string file, std::map<int,ftxui::Color> highlights, int focus_line) {
    std::ifstream filestream(file);
    std::string out;

    // Now, read lines of relevance
    int cur_line = 1;
    std::vector<ftxui::Element> lines;
    
    while (filestream) {
        std::getline(filestream, out);
        ftxui::Element text_elem = ftxui::text(out);
        if (cur_line == focus_line) text_elem |= ftxui::focus;
        if (highlights.contains(cur_line)) text_elem |= ftxui::bgcolor(highlights[cur_line]);
        lines.push_back(text_elem);
        cur_line++;
    }
    ShowLines(lines);
}

void ShowLines(std::vector<ftxui::Element> lines) {
    int selected = 0;
    int page_size = screen.dimy() - 10; // Size of output box
    int pages = lines.size() / page_size;
    std::map<int, std::vector<ftxui::Element>>  lines_per_page;
    for (size_t i = 0; i <= pages; i++) {
        auto end = std::min((i+1)*page_size, lines.size());
        lines_per_page[i] = std::vector<ftxui::Element>(lines.begin() + i*page_size, lines.begin() + end);
    }
    ftxui::MenuOption menu_options = {
        .entries = std::vector<std::string>{"Exit view", "Previous Page", "Next Page"},
        .selected = &selected,
        .on_enter = [&]() { screen.Exit(); }
    };
    ftxui::Component menu = ftxui::Menu(menu_options);
    int cur_page = 0;
    do {
        if (selected == 1) cur_page = std::max(0, cur_page - 1);
        else if (selected == 2) cur_page = std::min(pages, cur_page + 1);
        ftxui::Component render = ftxui::Renderer(
            menu, [&] {
            return ftxui::vbox({
                header,
                ftxui::text("Source Context (Page " + std::to_string(cur_page) + "/" + std::to_string(pages) + ")") | ftxui::center,
                ftxui::separator(),
                ftxui::vbox(lines_per_page[cur_page]),
                ftxui::filler(),
                ftxui::separator(),
                menu->Render(),
                ftxui::separator()
            });
            }
        );
        screen.Loop(render);
    } while (selected != 0);
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
        std::string file = msg["references"][choice-2]["file"].asString();
        int line = msg["references"][choice-2]["line"].asInt();
        ShowFile(file, {{line, ftxui::Color::Red}}, line);
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
