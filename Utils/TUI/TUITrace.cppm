module;

#include <format>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

export module TUITrace;
import LLVMModule;
import ContractPassUtility;
import InteractiveUtils;
import InteractiveCmds;
import TUIManager;

namespace TUITrace {

using ContractPassUtility::JumpTraceEntry;
using ContractPassUtility::TraceDB;
using InteractiveCmds::CmdContext;
using InteractiveCmds::CmdFrontend;
using InteractiveCmds::CmdResult;
using InteractiveCmds::CmdResultCode;
using InteractiveUtils::TraceBlock;

void ShowHistory(std::vector<std::pair<std::string, CmdResult>> const& history) {
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

export template<typename T>
bool ShowTrace(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr) {
    if (!trace) return false;
    std::vector<std::pair<std::string, CmdResult>> input_history = InteractiveCmds::LoadHistoryCache();
    std::map<JumpTraceEntry<T>*,int> sibling_select;
    std::map<JumpTraceEntry<T>*,bool> expand_select;
    bool expand_as_source = false;
    std::string last_res = "";
    while (true) {
        std::vector<TraceBlock<T>> trace_by_blocks = InteractiveUtils::GetTraceList(traceDB, trace, infoToStr, sibling_select);
        std::vector<ftxui::Element> full_trace;
        for (int i = 0; i < trace_by_blocks.size(); i++) {
            std::string trace_block_str = std::format("{:>3}: {:^{}} - {}", i, infoToStr(trace_by_blocks[i].first_entry->analysisInfo), InteractiveUtils::UI_ANALYSISINFO_PAD_SIZE, trace_by_blocks[i].trace_list);
            full_trace.push_back(ftxui::text(trace_block_str));
            if (expand_select.contains(trace_by_blocks[i].last_entry) && expand_select[trace_by_blocks[i].last_entry]) {
                std::vector<std::string> block_lines = InteractiveUtils::getBlockLines(trace_by_blocks[i], expand_as_source, infoToStr);
                std::vector<ftxui::Element> formatted_lines;
                for (std::string block_line : block_lines) {
                    formatted_lines.push_back(ftxui::text(std::format("{:>3}.{:0>3}", i, formatted_lines.size()) + block_line));
                }
                full_trace.insert(full_trace.end(), formatted_lines.begin(), formatted_lines.end());
            }
        }

        InteractiveUtils::WriteReanalyseModule(trace->loc->getModule());

        std::string input_command = TUIManager::RenderTxtEntry(full_trace, "JumpTrace", last_res);

        CmdContext<T> ctx{trace_by_blocks, sibling_select, expand_select, expand_as_source, input_history, infoToStr, TUIFrontend};
        CmdResult res = InteractiveCmds::ExecuteCommand(input_command, ctx);
        last_res = res.res;

        if (res.code == CmdResultCode::SUCCESS_EXIT) return false;
        if (res.code == CmdResultCode::SUCCESS_REANALYSE) {
            InteractiveCmds::SaveHistoryCache(ctx.history);
            return true;
        }
    }
}

} // namespace TUITrace
