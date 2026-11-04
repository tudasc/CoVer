module;

#include <string>
#include <vector>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

export module TUIMCP;
import TUIManager;
import MCPServer;
import MCPControl;

// Terminal view serving tool calls of MCP clients. Tool calls are posted to the view's event loop (see MCPControl::Start).
// While MCP mode is active (MCPControl::Active), the results screen and the debugger show this view instead of their own.
namespace TUIMCP {

std::string const REFUSAL = "CoVer's terminal UI only runs tool calls in MCP mode. Ask the user to start it in CoVer, "
                            "with the mcp command of the trace debugger or the MCP entry of the report list, then try again.";

// Starts the server if needed and enters MCP mode, returns an error message on failure
export std::string Activate() {
    return MCPControl::Start(TUIManager::Post);
}

ftxui::Element Document(std::string const& state) {
    std::vector<MCPServer::Activity> const& log = MCPControl::Log();
    std::vector<ftxui::Element> lines;
    if (log.empty()) lines.push_back(ftxui::text("No tool calls yet.") | ftxui::dim);
    for (MCPServer::Activity const& entry : log)
        lines.push_back(ftxui::paragraph("> " + entry.summary) | ftxui::color(entry.error ? ftxui::Color::Red : ftxui::Color::Blue));
    lines.back() = lines.back() | ftxui::focus; // Show the latest entries
    return ftxui::vbox({
        TUIManager::getHeader("MCP Mode"),
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
export void Show(std::string const& state) {
    if (MCPControl::PendingAction().kind != MCPControl::Action::Kind::NONE) return;
    ftxui::App& screen = TUIManager::Screen();
    // Tool calls run while the view is shown, it closes once one of them requests navigation
    MCPControl::ScreenScope screen_scope([&](std::vector<MCPServer::Activity> const&) {
        if (MCPControl::PendingAction().kind != MCPControl::Action::Kind::NONE) screen.Exit();
    });
    ftxui::Component view = ftxui::CatchEvent(ftxui::Renderer([&] { return Document(state); }), [&](ftxui::Event event) {
        if (event != ftxui::Event::Escape && event != ftxui::Event::Character('q')) return false;
        MCPControl::Leave(REFUSAL);
        screen.Exit();
        return true;
    });
    TUIManager::Post(MCPControl::RunWaiting); // Tool calls may have arrived before the view was shown
    screen.Loop(view);
}

} // namespace TUIMCP
