module;

#include <algorithm>
#include <expected>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

export module TUIChat;
import TUIManager;
import LLMChat;
import LLMClient;

// Terminal frontend of the LLM chat
namespace TUIChat {

using LLMChat::ChatEntry;

// One element per line, so the transcript can be scrolled line by line
void AddEntry(std::vector<ftxui::Element>& elems, ChatEntry const& entry) {
    auto addLines = [&](std::string const& text, ftxui::Decorator decorator) {
        std::istringstream stream(text);
        for (std::string line; std::getline(stream, line);)
            elems.push_back((line.empty() ? ftxui::text("") : ftxui::paragraph(line)) | decorator);
    };
    switch (entry.kind) {
        case ChatEntry::Kind::USER:
            elems.push_back(ftxui::text("You") | ftxui::bold | ftxui::color(ftxui::Color::Cyan));
            addLines(entry.text, ftxui::nothing);
            break;
        case ChatEntry::Kind::ASSISTANT:
            elems.push_back(ftxui::text("Assistant") | ftxui::bold | ftxui::color(ftxui::Color::Green));
            addLines(entry.text, ftxui::nothing);
            break;
        case ChatEntry::Kind::THINKING:
            addLines("(thinking) " + entry.text, ftxui::dim);
            break;
        case ChatEntry::Kind::TOOL:
            addLines("  > " + entry.text, ftxui::color(ftxui::Color::Blue));
            return; // Tool calls are listed without spacing
        case ChatEntry::Kind::INFO:
            addLines(entry.text, ftxui::color(ftxui::Color::Yellow));
            break;
        case ChatEntry::Kind::ERROR:
            addLines("Error: " + entry.text, ftxui::color(ftxui::Color::Red));
            break;
    }
    elems.push_back(ftxui::text(""));
}

// Shows the chat until closed with Escape, which cancels a running request
void ShowChat(LLMChat::ChatSession& chat) {
    ftxui::App& screen = TUIManager::Screen();
    std::string input;
    int scroll = -1; // Focused line, -1 to follow the end of the transcript
    int num_lines = 0;

    ftxui::InputOption input_options = {
        .content = &input,
        .placeholder = "Ask about the trace, Enter to send",
        .multiline = false,
        .on_enter = [&] {
            if (chat.Busy() || input.empty()) return;
            std::string message = input;
            input.clear();
            scroll = -1;
            chat.Send(message);
        }
    };
    ftxui::Component input_comp = ftxui::Input(input_options);

    ftxui::Component render = ftxui::CatchEvent(ftxui::Renderer(input_comp, [&] {
        if (chat.Busy()) screen.RequestAnimationFrame(); // Render again soon, to update the waiting time in the status
        std::vector<ftxui::Element> lines;
        if (chat.Transcript().empty()) {
            lines.push_back(ftxui::paragraph("Ask the LLM about this trace, e.g. why the error is reported or whether it is a false positive. "
                                             "It can inspect the trace, IR and source code, and add annotations. "
                                             "Annotations it adds are recorded in the command history and shown in the trace once you close the chat. "
                                             "Run \"reanalyse\" afterwards to check their effect.") | ftxui::dim);
        }
        for (ChatEntry const& entry : chat.Transcript()) AddEntry(lines, entry);
        if (chat.Busy()) lines.push_back(ftxui::text("Waiting for the model...") | ftxui::dim);
        num_lines = lines.size();
        int focused = scroll < 0 ? num_lines - 1 : std::clamp(scroll, 0, num_lines - 1);
        lines[focused] = lines[focused] | ftxui::focus;
        return ftxui::vbox({
            TUIManager::getHeader("LLM Chat"),
            ftxui::vbox(lines) | ftxui::vscroll_indicator | ftxui::yframe | ftxui::flex,
            ftxui::separator(),
            ftxui::text(chat.Status()) | ftxui::dim,
            ftxui::hbox(ftxui::text(">>> "), input_comp->Render()),
            ftxui::separator(),
            ftxui::text(chat.Busy() ? "Esc: cancel the request and close | PgUp/PgDn: scroll" : "Enter: send | Esc: close chat | PgUp/PgDn: scroll"),
        });
    }), [&](ftxui::Event e) {
        if (e == ftxui::Event::Escape) {
            screen.Exit();
            return true;
        }
        int step = 0;
        if (e == ftxui::Event::PageUp) step = -10;
        else if (e == ftxui::Event::PageDown) step = 10;
        else if (e == ftxui::Event::ArrowUp || (e.is_mouse() && e.mouse().button == ftxui::Mouse::WheelUp)) step = -1;
        else if (e == ftxui::Event::ArrowDown || (e.is_mouse() && e.mouse().button == ftxui::Mouse::WheelDown)) step = 1;
        if (step == 0) return false;
        scroll = std::max(0, (scroll < 0 ? num_lines - 1 : scroll) + step);
        if (scroll >= num_lines - 1) scroll = -1;
        return true;
    });

    screen.Loop(render);
    chat.Cancel();
}

// Form for the connection settings, the error tells why it is shown. False if cancelled.
bool EditSettings(LLMClient::Config& config, std::string const& error) {
    ftxui::App& screen = TUIManager::Screen();
    std::string url = config.base_url, model = config.model, api_key = config.api_key, effort = config.effort;
    std::string max_tokens = config.max_tokens > 0 ? std::to_string(config.max_tokens) : "";
    std::string message = error;
    bool saved = false;

    auto submit = [&] {
        std::expected<LLMClient::Config, std::string> made = LLMClient::MakeConfig(url, model, api_key, effort, max_tokens);
        if (!made) {
            message = made.error();
            return;
        }
        config = *made;
        saved = true;
        screen.Exit();
    };
    std::vector<std::unique_ptr<int>> cursors; // Start at the end of the prefilled text
    auto field = [&](std::string& content, std::string const& placeholder, bool password = false) {
        cursors.push_back(std::make_unique<int>(content.size()));
        ftxui::InputOption options = ftxui::InputOption::Default();
        options.content = &content;
        options.cursor_position = cursors.back().get();
        options.placeholder = placeholder;
        options.password = password;
        options.multiline = false;
        options.on_enter = submit;
        return ftxui::Input(options);
    };
    std::vector<std::pair<std::string, ftxui::Component>> fields = {
        {"Server URL", field(url, "e.g. http://localhost:8000/v1 (vLLM), http://localhost:11434/v1 (Ollama), http://localhost:8080/v1 (llama.cpp)")},
        {"Model", field(model, "name of the model, as the server knows it")},
        {"API key", field(api_key, "only if the server requires one", true)},
        {"Reasoning effort", field(effort, "server default, e.g. low, medium, high")},
        {"Max response tokens", field(max_tokens, "server default")},
    };
    ftxui::Component save = ftxui::Button("Save and connect", submit, ftxui::ButtonOption::Ascii());
    ftxui::Component cancel = ftxui::Button("Cancel", [&] { screen.Exit(); }, ftxui::ButtonOption::Ascii());
    ftxui::Components rows;
    for (std::pair<std::string, ftxui::Component> const& f : fields) rows.push_back(f.second);
    rows.push_back(ftxui::Container::Horizontal({save, cancel}));
    ftxui::Component container = ftxui::Container::Vertical(rows);

    ftxui::Component render = ftxui::CatchEvent(ftxui::Renderer(container, [&] {
        std::vector<ftxui::Element> lines = {
            TUIManager::getHeader("LLM Chat Settings"),
            ftxui::paragraph("The chat works with servers implementing the OpenAI Chat Completions API, e.g. vLLM, llama.cpp, Ollama or SGLang. "
                             "The model must support tool use. The settings are saved in your CoVer user settings, which only you can read.") | ftxui::dim,
            ftxui::text(""),
        };
        if (!message.empty()) {
            std::istringstream stream(message);
            for (std::string line; std::getline(stream, line);) lines.push_back(ftxui::paragraph(line) | ftxui::color(ftxui::Color::Red));
            lines.push_back(ftxui::text(""));
        }
        for (std::pair<std::string, ftxui::Component> const& f : fields)
            lines.push_back(ftxui::hbox({ftxui::text(f.first) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 22), f.second->Render() | ftxui::flex}));
        lines.push_back(ftxui::text(""));
        lines.push_back(ftxui::hbox({save->Render(), ftxui::text("  "), cancel->Render()}));
        lines.push_back(ftxui::filler());
        lines.push_back(ftxui::separator());
        lines.push_back(ftxui::text("Enter: save and connect | Tab/arrows: move | Esc: cancel"));
        return ftxui::vbox(lines);
    }), [&](ftxui::Event e) {
        if (e != ftxui::Event::Escape) return false;
        screen.Exit();
        return true;
    });
    screen.Loop(render);
    return saved;
}

// Shows the running connection test until it is done, false if cancelled
bool WaitForTest(LLMChat::ConnectionTest const& test, LLMClient::Config const& config) {
    ftxui::App& screen = TUIManager::Screen();
    bool cancelled = false;
    ftxui::Component render = ftxui::CatchEvent(ftxui::Renderer([&] {
        if (test.Result()) screen.Exit();
        else screen.RequestAnimationFrame(); // Render again soon, to notice the end of the test
        return ftxui::vbox({
            TUIManager::getHeader("LLM Chat"),
            ftxui::paragraph("Testing the connection to " + config.model + " at " + config.base_url + "..."),
            ftxui::paragraph("This can take a while if the server has to load the model first.") | ftxui::dim,
            ftxui::filler(),
            ftxui::separator(),
            ftxui::text("Esc: cancel"),
        });
    }), [&](ftxui::Event e) {
        if (e != ftxui::Event::Escape) return false;
        cancelled = true;
        screen.Exit();
        return true;
    });
    screen.Loop(render);
    return !cancelled;
}

// Tests the connection and shows the chat, asking for the settings first if needed
export void OpenChat(LLMChat::ChatSession& chat) {
    LLMChat::Open(chat, {.settings = EditSettings, .testing = WaitForTest, .chat = [&] { ShowChat(chat); }});
}

// Lets the user edit and test the settings, returns the outcome
export std::string OpenSettings() {
    return LLMChat::Configure({.settings = EditSettings, .testing = WaitForTest});
}

} // namespace TUIChat
