module;

#include <format>
#include <string>
#include <utility>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

export module TUITrace;
import InteractiveUtils;
import InteractiveCmds;
import InteractiveSession;
import TUIManager;

namespace TUITrace {

using InteractiveCmds::CmdFrontend;
using InteractiveSession::CmdResult;
using InteractiveSession::CmdResultCode;
using InteractiveSession::History;

void ShowHistory(History const& history) {
    int constexpr res_size = 17;
    std::vector<ftxui::Element> lines;
    lines.push_back(ftxui::hbox({
        ftxui::text("#"),
        ftxui::separator(),
        ftxui::text("Return Code") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, res_size),
        ftxui::separator(),
        ftxui::text("Command")
    }));
    lines.push_back(ftxui::separator());
    for (int i = 0; i < history.size(); i++) {
        ftxui::Element result_str;
        switch (history[i].second.code) {
            case CmdResultCode::INVALID_INPUT:
                result_str = ftxui::text("Invalid Input") | ftxui::bgcolor(ftxui::Color::Yellow);
                break;
            case CmdResultCode::INVALID_SYNTAX:
                result_str = ftxui::text("Invalid Syntax") | ftxui::bgcolor(ftxui::Color::Yellow);
                break;
            case CmdResultCode::UNKNOWN_COMMAND:
                result_str = ftxui::text("Unknown Command") | ftxui::bgcolor(ftxui::Color::Red);
                break;
            default:
                result_str = ftxui::text("Success");
                break;
        }
        result_str |= ftxui::size(ftxui::WIDTH, ftxui::EQUAL, res_size);
        lines.push_back(ftxui::hbox({
            ftxui::text(std::to_string(i)),
            ftxui::separator(),
            result_str,
            ftxui::separator(),
            ftxui::text(history[i].first)
        }));
    }
    TUIManager::ShowLines(lines, "Command History");
}

CmdFrontend const TUIFrontend = {
    .showLines = [](std::vector<std::string> const& lines, std::string const& title) {
        std::vector<ftxui::Element> elems;
        for (std::string const& line : lines) elems.push_back(ftxui::text(line));
        TUIManager::ShowLines(elems, title);
    },
    .selectMultiple = TUIManager::RenderMultiMenu,
    .showHistory = ShowHistory,
};

export bool ShowTrace(InteractiveSession::TraceSession& session) {
    std::string last_res = "";
    while (true) {
        std::vector<ftxui::Element> full_trace;
        for (InteractiveSession::TraceBlockView const& block : session.Blocks()) {
            full_trace.push_back(ftxui::text(std::format("{:>3}: {:^{}} - {}", block.index, block.first.info, InteractiveUtils::UI_ANALYSISINFO_PAD_SIZE, block.description)));
            if (!block.expanded) continue;
            for (InteractiveSession::BlockLine const& line : session.BlockLines(block.index, session.ExpandAsSource())) {
                full_trace.push_back(ftxui::text(std::format("{:>3}.{:0>3}{}", block.index, line.instr, line.text)));
            }
        }

        std::string input_command = TUIManager::RenderTxtEntry(full_trace, "JumpTrace", last_res);

        CmdResult res = InteractiveCmds::ExecuteCommand(session, input_command, TUIFrontend);
        last_res = res.res;

        if (res.code == CmdResultCode::SUCCESS_EXIT) return false;
        if (res.code == CmdResultCode::SUCCESS_REANALYSE) return true;
    }
}

} // namespace TUITrace
