#include "TUIManager.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <functional>
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

ftxui::Element getHeader(std::string cur_title) {
    ftxui::Element header = ftxui::vbox({
        ftxui::separator(),
        ftxui::hbox({ftxui::text("CoVer (Interactive mode) "), ftxui::separator(), ftxui::text(" " + cur_title), ftxui::filler()}),
        ftxui::separator()
    });
    return header;
}

bool scrollableEventHdlr(ftxui::Event e, size_t num_lines, int offset, size_t* cur_focus) {
    // Avoid "empty scrolling"
    int lines_shown = screen.dimy() - offset; // Number of lines of the trace shown
    size_t min_focus = (lines_shown / 2) - 1;
    size_t max_focus = num_lines - (lines_shown / 2) - 1;
    *cur_focus = std::clamp(*cur_focus, min_focus, max_focus);

    if (e == ftxui::Event::ArrowUp || (e.is_mouse() && e.mouse().button == ftxui::Mouse::WheelUp)) {
        *cur_focus = std::max(min_focus, *cur_focus - 1);
        return true;
    }
    if (e == ftxui::Event::ArrowDown || (e.is_mouse() && e.mouse().button == ftxui::Mouse::WheelDown)) {
        *cur_focus = std::min(max_focus, *cur_focus + 1);
        return true;
    }
    return false;
}

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
            getHeader(title),
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
        full_lines.push_back(ftxui::text(line));
    }
    size_t offset = 7; // Number of lines taken by decorations, input, etc. excluding trace itself
    size_t cur_focus = 0;
    std::function<bool(ftxui::Event)> scrollableEventHdlr_inst = std::bind(scrollableEventHdlr, std::placeholders::_1, full_lines.size(), offset, &cur_focus);
    ftxui::Component render = ftxui::CatchEvent(ftxui::Renderer(
        input_comp, [&] {
           return ftxui::vbox({
            getHeader(title),
            ftxui::vbox(full_lines) | ftxui::focusPosition(0, cur_focus) | ftxui::vscroll_indicator | ftxui::yframe | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, screen.dimy() - offset),
            ftxui::separator(),
            ftxui::text(last_res),
            ftxui::hbox(ftxui::text(">>> "), input_comp->Render())
           });
        }
    ), scrollableEventHdlr_inst);
    render->OnEvent(ftxui::Event()); // Trigger onEvent once to let cur_focus be computed
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
            getHeader(title),
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
            getHeader("Contract Details: " + C.F->getName().str()),
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
        ftxui::Element text_elem = ftxui::text(std::format("{:>4} | {}", cur_line, out));
        if (cur_line == focus_line) text_elem |= ftxui::focus;
        if (highlights.contains(cur_line)) text_elem |= ftxui::bgcolor(highlights[cur_line]);
        lines.push_back(text_elem);
        cur_line++;
    }
    ShowLines(lines, "Source Preview", focus_line);
}

void ShowLines(std::vector<ftxui::Element> lines, std::string title, int focus) {
    int selected = 0;
    ftxui::MenuOption menu_options = {
        .entries = std::vector<std::string>{"Exit view"},
        .selected = &selected,
        .on_enter = [&]() { screen.Exit(); }
    };
    ftxui::Component menu = ftxui::Menu(menu_options);

    int offset = 6;
    size_t cur_focus = focus;
    std::function<bool(ftxui::Event)> scrollableEventHdlr_inst = std::bind(scrollableEventHdlr, std::placeholders::_1, lines.size(), offset, &cur_focus);
    ftxui::Component render = ftxui::CatchEvent(ftxui::Renderer(
        menu, [&] {
        return ftxui::vbox({
            getHeader(title),
            ftxui::vbox(lines) | ftxui::focusPosition(0, cur_focus) | ftxui::vscroll_indicator | ftxui::yframe | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, screen.dimy() - offset),
            ftxui::filler(),
            ftxui::separator(),
            menu->Render(),
            ftxui::separator()
        });
        }
    ), scrollableEventHdlr_inst);
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
            getHeader("Report Details: " + msg["type"].asString()),
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
