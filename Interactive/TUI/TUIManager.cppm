module;

#include "ContractTree.hpp"

export module TUIManager;
import std;
import ftxui;
import AnalysisReports;
import LLVMModule;
import ErrorMessage;
import InteractiveUtils;
import MCPServer;
import MCPControl;
import LLMChat;

using namespace ContractTree;

using namespace llvm;

namespace TUIManager {

using Contract = AnalysisReports::Contract;

ftxui::Decorator FulfillmentColor(Fulfillment f) {
    switch (f) {
        case Fulfillment::FULFILLED: return ftxui::bgcolor(ftxui::Color::Green);
        case Fulfillment::UNKNOWN: return ftxui::bgcolor(ftxui::Color::Yellow);
        case Fulfillment::BROKEN: return ftxui::bgcolor(ftxui::Color::Red);
    }
}

static ftxui::App screen = ftxui::App::FullscreenPrimaryScreen();

export ftxui::App& Screen() { return screen; }

// Runs a task on the TUI thread, usable from any thread. It runs in the next screen's event loop, and the screen is redrawn afterwards.
export void Post(std::function<void()> task) {
    screen.Post(std::move(task));
    screen.PostEvent(ftxui::Event::Custom); // Tasks do not redraw the screen by themselves, events do
}

export ftxui::Element getHeader(std::string cur_title) {
    ftxui::Element header = ftxui::vbox({
        ftxui::separator(),
        ftxui::hbox({ftxui::text("CoVer (Interactive mode) "), ftxui::separator(), ftxui::text(" " + cur_title), ftxui::filler()}),
        ftxui::separator()
    });
    return header;
}

static bool scrollableEventHdlr(ftxui::Event e, int num_lines, int offset, int& cur_focus) {
    // Avoid "empty scrolling", signed as lists may be shorter than the view
    int half_shown = (screen.dimy() - offset) / 2; // Half the number of lines of the trace shown
    int min_focus = std::max(0, half_shown - 1);
    int max_focus = std::max(min_focus, num_lines - half_shown - 1);
    cur_focus = std::clamp(cur_focus, min_focus, max_focus);

    if (e == ftxui::Event::ArrowUp || (e.is_mouse() && e.mouse().button == ftxui::Mouse::WheelUp)) {
        cur_focus = std::max(min_focus, cur_focus - 1);
        return true;
    }
    if (e == ftxui::Event::ArrowDown || (e.is_mouse() && e.mouse().button == ftxui::Mouse::WheelDown)) {
        cur_focus = std::min(max_focus, cur_focus + 1);
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
            menu->Render() | ftxui::vscroll_indicator | ftxui::yframe | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, screen.dimy() - 5),
            ftxui::separator()
           });
        }
    );
    screen.Loop(render);
    return *menu_options.selected;
}

export std::vector<std::string> RenderMultiMenu(std::vector<std::string> choices, std::string title, std::set<std::string> already_selected) {
    std::map<std::string, bool> sel_choices;
    std::map<std::string, bool> visible;
    std::vector<ftxui::Component> elems;
    for (std::string const& choice : choices) {
        sel_choices[choice] = already_selected.contains(choice);
        visible[choice] = true;
        elems.push_back(ftxui::Maybe(ftxui::Checkbox(choice, &sel_choices[choice]), &visible[choice]));
    }
    ftxui::Component list = ftxui::Container::Vertical(elems);

    std::string search_str;
    ftxui::InputOption search_options = {
        .content = &search_str,
        .placeholder = "Type to search...",
        .multiline = false,
        .on_change = [&]() {
            for (std::string const& choice : choices) {
                std::string input_lower = ((StringRef)search_str).lower();
                std::string choice_lower = ((StringRef)choice).lower();
                visible[choice] = input_lower.empty() || (choice_lower.find(input_lower) != std::string::npos);
            }
        }
    };
    ftxui::Component search_input = ftxui::Input(search_options);

    ftxui::Component render = ftxui::CatchEvent(ftxui::Renderer(
        list, [&] {
           return ftxui::vbox({
            getHeader(title),
            list->Render() | ftxui::vscroll_indicator | ftxui::yframe | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, screen.dimy() - 8),
            ftxui::separator(),
            search_input->Render(),
            ftxui::separator(),
            ftxui::text("Space to select, enter to confirm"),
            ftxui::separator()
           });
        }
    ), [&](ftxui::Event const& e) {
        if (e == ftxui::Event::Return) {screen.Exit(); return true;}
        if (e == ftxui::Event::Character(' ')) { return list->OnEvent(e); }
        if (e.is_character() || e == ftxui::Event::Backspace || e == ftxui::Event::ArrowLeft || e == ftxui::Event::ArrowRight) { return search_input->OnEvent(e); }
        return false;
    });
    screen.Loop(render);
    std::vector<std::string> act_choices;
    for (std::pair<std::string, bool> act_choice : sel_choices) {
        if (act_choice.second) act_choices.push_back(act_choice.first);
    }
    return act_choices;
}

export std::string RenderTxtEntry(std::vector<ftxui::Element> lines, std::string title, std::string last_res) {
    std::string input_str;
    ftxui::InputOption input_options = {
        .content = &input_str,
        .placeholder = "Type \"help\" for help",
        .multiline = false,
        .on_enter = [&]() { screen.Exit(); }
    };
    ftxui::Component input_comp = ftxui::Input(input_options);
    int offset = 7; // Number of lines taken by decorations, input, etc. excluding trace itself
    int cur_focus = 0;
    std::function<bool(ftxui::Event)> scrollableEventHdlr_inst = [&](ftxui::Event e) { return scrollableEventHdlr(e, lines.size(), offset, cur_focus); };
    ftxui::Component render = ftxui::CatchEvent(ftxui::Renderer(
        input_comp, [&] {
           return ftxui::vbox({
            getHeader(title),
            ftxui::vbox(lines) | ftxui::focusPosition(0, cur_focus) | ftxui::vscroll_indicator | ftxui::yframe | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, screen.dimy() - offset),
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

// top is the reported formula, Form one of its subformulas
bool ShowContractFormula(Contract const& C, ContractTree::ContractFormula const& top, std::shared_ptr<ContractTree::ContractFormula> const& Form, AnalysisReports::FormulaReports const& reports, AnalysisReports::FormulaDebugHandlers& debugHandlers, std::string const& title) {
    int selected = 0;
    bool debuggable = debugHandlers.contains(Form.get());
    std::vector<std::string> menu_entries = {"Exit debugger"};
    if (debuggable) menu_entries.push_back("Inspect formula worklist analysis");
    for (std::shared_ptr<ContractFormula> const& subform : Form->Children) {
        menu_entries.push_back("Inspect " + FulfillmentStr(*subform->Status) + " subformula: " + subform->ExprStr);
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
    if (selected == 0) return false;
    if (debuggable) return debugHandlers[Form.get()](AnalysisReports::DebugContext(C, top, *Form, reports));
    return ShowContractFormula(C, top, Form->Children[selected - 1], reports, debugHandlers, title);
}

bool ShowContractDetails(Contract const& C, AnalysisReports::FormulaReports const& reports, AnalysisReports::FormulaDebugHandlers& debugHandlers) {
    std::vector<std::string> menu_entries = {"Back to Listing"};
    std::vector<std::shared_ptr<ContractFormula>> violated_formulas;
    for (auto const& [kind, scope] : {std::pair{"Precondition", C.Data.Pre}, std::pair{"Postcondition", C.Data.Post}}) {
        if (!scope) continue;
        for (std::shared_ptr<ContractFormula> const& form : scope->Children) {
            if (*form->Status != Fulfillment::BROKEN) continue;
            menu_entries.push_back(std::format("Inspect {} {} Subformula: {}", FulfillmentStr(*form->Status), kind, form->ExprStr));
            violated_formulas.push_back(form);
        }
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
            ftxui::text(C.ContractString),
            ftxui::separator(),
            menu->Render(),
            ftxui::separator()
           });
        }
    );
    screen.Loop(render);
    if (*menu_options.selected == 0) return false;
    std::shared_ptr<ContractFormula> const& form = violated_formulas[*menu_options.selected-1];
    return ShowContractFormula(C, *form, form, reports, debugHandlers, "Inspecting Contract Subformula for Function: " + C.F->getName().str());
}

export void StartMenu(std::vector<AnalysisReports::Contract> const& Contracts, AnalysisReports::FormulaReports const& reports, AnalysisReports::FormulaDebugHandlers& debugHandlers, double parse_time) {
    std::vector<std::string> contr_funcs;
    contr_funcs.push_back("Start Analysis");
    for (AnalysisReports::Contract const& contract : Contracts) {
        contr_funcs.push_back("Read Contract: " + contract.F->getName().str());
    }

    int choice;
    do {
        choice = RenderMenu(contr_funcs, InteractiveUtils::startMenuTitle(Contracts.size(), parse_time));
        // Can ignore ShowContractDetails ret here, no way for reanalyse command as no analyses started yet
        if (choice != 0) ShowContractDetails(Contracts[choice-1], reports, debugHandlers);
    } while (choice != 0);
}

export void ShowLines(std::vector<ftxui::Element> lines, std::string title, int focus = 0) {
    int selected = 0;
    ftxui::MenuOption menu_options = {
        .entries = std::vector<std::string>{"Exit view"},
        .selected = &selected,
        .on_enter = [&]() { screen.Exit(); }
    };
    ftxui::Component menu = ftxui::Menu(menu_options);

    int offset = 6;
    int cur_focus = focus;
    std::function<bool(ftxui::Event)> scrollableEventHdlr_inst = [&](ftxui::Event e) { return scrollableEventHdlr(e, lines.size(), offset, cur_focus); };
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

export void ShowMessage(std::string const& title, std::string const& text) {
    ShowLines({ftxui::paragraph(text)}, title);
}

// Asks for a line of text, prefilled with initial. Empty if cancelled.
export std::optional<std::string> PromptText(std::string const& title, std::string const& description, std::string initial) {
    std::string input = std::move(initial);
    int cursor = input.size(); // Start at the end of the prefilled text
    bool confirmed = false;
    ftxui::InputOption options = ftxui::InputOption::Default();
    options.content = &input;
    options.cursor_position = &cursor;
    options.multiline = false;
    options.on_enter = [&] {
        confirmed = true;
        screen.Exit();
    };
    ftxui::Component input_comp = ftxui::Input(options);
    ftxui::Component render = ftxui::CatchEvent(ftxui::Renderer(input_comp, [&] {
        return ftxui::vbox({
            getHeader(title),
            ftxui::paragraph(description),
            ftxui::text(""),
            ftxui::hbox(ftxui::text(">>> "), input_comp->Render()),
            ftxui::filler(),
            ftxui::separator(),
            ftxui::text("Enter: confirm | Esc: cancel"),
        });
    }), [&](ftxui::Event e) {
        if (e != ftxui::Event::Escape) return false;
        screen.Exit();
        return true;
    });
    screen.Loop(render);
    if (!confirmed) return std::nullopt;
    return input;
}

// Asks for a file and exports the log of the LLM chat to it
export void ExportChatLog() {
    std::optional<std::string> path = PromptText("Export Chat Log", "File to write the log of the LLM chat to, as JSON. It includes the full outputs of the tool calls.", LLMChat::EXPORT_FILE);
    if (!path || path->empty()) return;
    std::string err = LLMChat::ExportConversation(*path);
    ShowMessage("Export Chat Log", err.empty() ? "Exported the chat log to " + *path : err);
}

// MCP mode: a view serving tool calls of MCP clients. Tool calls are posted to the view's event loop (see MCPControl::Start).
// While MCP mode is active (MCPControl::Active), the results screen and the debugger show this view instead of their own.

std::string const MCP_REFUSAL = "CoVer's terminal UI only runs tool calls in MCP mode. Ask the user to start it in CoVer, "
                                "with the mcp command of the trace debugger or the MCP entry of the report list, then try again.";

// Starts the server if needed and enters MCP mode, returns an error message on failure
export std::string ActivateMCP() {
    return MCPControl::Start(Post);
}

ftxui::Element MCPDocument(std::string const& state) {
    std::vector<MCPServer::Activity> const& log = MCPControl::Log();
    std::vector<ftxui::Element> lines;
    if (log.empty()) lines.push_back(ftxui::text("No tool calls yet.") | ftxui::dim);
    for (MCPServer::Activity const& entry : log)
        lines.push_back(ftxui::paragraph("> " + entry.summary) | ftxui::color(entry.error ? ftxui::Color::Red : ftxui::Color::Blue));
    lines.back() = lines.back() | ftxui::focus; // Show the latest entries
    return ftxui::vbox({
        getHeader("MCP Mode"),
        ftxui::text("URL:    " + MCPControl::Url()) | ftxui::bold,
        ftxui::text("Header: " + MCPControl::AuthHeader()) | ftxui::bold,
        ftxui::paragraph("Set up your MCP client once with this URL and header (Streamable HTTP transport, see the documentation). "
                         "The token is kept in your CoVer user settings, remove it there to generate a new one (see the documentation). "
                         "MCP clients can now work through the reports: open their traces, add annotations and reanalyse. "
                         "MCP mode stays active across reanalyses."),
        ftxui::text(state) | ftxui::dim,
        ftxui::separator(),
        ftxui::vbox(lines) | ftxui::vscroll_indicator | ftxui::yframe | ftxui::flex,
        ftxui::separator(),
        ftxui::text(std::to_string(log.size()) + " tool calls | Esc or q: leave MCP mode and take over, tool calls are refused until MCP mode is started again"),
    });
}

// Serves tool calls until one requests navigation (see MCPControl::PendingAction), or the user leaves MCP mode with Esc or q.
// state describes what is shown, e.g. the open trace.
export void ShowMCP(std::string const& state) {
    if (MCPControl::PendingAction().kind != MCPControl::Action::Kind::NONE) return;
    // Tool calls run while the view is shown, it closes once one of them requests navigation
    MCPControl::ScreenScope screen_scope([&](std::vector<MCPServer::Activity> const&) {
        if (MCPControl::PendingAction().kind != MCPControl::Action::Kind::NONE) screen.Exit();
    });
    ftxui::Component view = ftxui::CatchEvent(ftxui::Renderer([&] { return MCPDocument(state); }), [&](ftxui::Event event) {
        if (event != ftxui::Event::Escape && event != ftxui::Event::Character('q')) return false;
        MCPControl::Leave(MCP_REFUSAL);
        screen.Exit();
        return true;
    });
    Post(MCPControl::RunWaiting); // Tool calls may have arrived before the view was shown
    screen.Loop(view);
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

bool ShowViolationDetails(std::shared_ptr<ContractFormula> const& form, AnalysisReports::FormulaReports const& reports) {
    std::vector<std::string> choices;
    choices.push_back("Back to Listing");
    choices.push_back("Launch Debugger");

    std::vector<FileReference> refs;
    for (ErrorMessage const& err : reports.at(form.get())) {
        for (FileReference const& ref : err.references) {
            choices.push_back(std::format("View Reference: {}:{}:{}", ref.file, ref.line, ref.column));
            refs.push_back(ref);
        }
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
            getHeader("Report Details: " + reports.at(form.get())[0].text),
            ftxui::text("Full Report:"),
            ftxui::text(reports.at(form.get())[0].text),
            ftxui::separator(),
            menu->Render(),
            ftxui::separator()
           });
        }
    );
    screen.Loop(render);
    int choice = *menu_options.selected;
    while (choice > 1) { // File Reference Inspection
        int line = refs[choice-2].line;
        ShowFile(refs[choice-2].file, {{line, ftxui::Color::Red}}, line);
        screen.Loop(render);
        choice = *menu_options.selected;
    }
    return choice == 1; // Debug or no debug
}

// The results of the analysis: shows the results menu, or the MCP view while MCP mode is active, and opens the reports
// requested by tool calls. Returns whether to reanalyse.
export bool ResultsScreen(std::vector<InteractiveUtils::ReportedFormula> const& formulas, AnalysisReports::FormulaReports const& reports, AnalysisReports::FormulaDebugHandlers& debugHandlers, double runtime) {
    std::string title = InteractiveUtils::resultsTitle(!formulas.empty(), runtime);
    while (true) {
        // Also requested from the debugger, once MCP mode was started there
        if (MCPControl::PendingAction().kind == MCPControl::Action::Kind::OPEN_REPORT) {
            if (MCPControl::OpenPendingReport()) return true;
            continue;
        }
        if (MCPControl::Active()) {
            ShowMCP(std::format("Results of the analysis: {} reports, no trace open", formulas.size()));
            continue;
        }
        // A chat of this session can be exported once it was used, possibly before the reanalysis
        bool has_chat = LLMChat::HasConversation();
        std::vector<std::string> entries = {"Exit Tool", "Start MCP mode"};
        if (has_chat) entries.push_back("Export LLM chat log");
        std::size_t first_report = entries.size();
        for (InteractiveUtils::ReportedFormula const& formula : formulas) entries.push_back(formula.Summary());
        int choice = RenderMenu(entries, title);
        if (choice == 0) return false;
        if (choice == 1) {
            if (std::string err = ActivateMCP(); !err.empty()) ShowMessage("MCP Mode", err);
            continue;
        }
        if (has_chat && choice == 2) {
            ExportChatLog();
            continue;
        }
        InteractiveUtils::ReportedFormula const& formula = formulas[choice - first_report];
        if (!ShowViolationDetails(formula.formula, reports)) continue;
        if (ShowContractDetails(*formula.contract, reports, debugHandlers)) return true;
    }
}

}
