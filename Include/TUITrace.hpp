#pragma once

#include <algorithm>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/color.hpp>
#include <vector>
#include "ContractPassUtility.hpp"
#include "TUICmds.hpp"

namespace TUITrace {

template<typename T>
using JumpTraceEntry = ContractPassUtility::JumpTraceEntry<T>;

template<typename T>
using TraceDB = ContractPassUtility::TraceDB<T>;

template<typename T>
std::vector<CmdInfo<T>> GetTraceCommands() {
    return {
        {"collapse",      "[block num]",                                              "Hide IR for block, making it unavailable for annotation", 1, CmdCollapse<T>},
        {"expand",        "[block num]",                                              "Show IR for block, and make available for annotation", 1, CmdExpand<T>},
        {"child",         "[block num] [child num]",                                  "Show different predecessor blocks", 2, CmdChild<T>},
        {"view",          "[source|ir] [block num]",                                  "Present a preview of IR or a source code approximation of a block", 2, CmdView<T>},
        {"fp-target",     "[block num] [instr num]",                                  "Annotate possible function pointer target(s)", 2, CmdFpTarget<T>},
        {"alias-create",  "[yes|no] [block num 1] [value 1] [block num 2] [value 2]", "Annotate if two values should/should not alias. This automatically creates a new alias group.", 5, CmdAliasCreate<T>},
        {"alias-add",     "[group num] [block num] [value]",                          "Add a value to an existing alias group", 3, CmdAliasAdd<T>},
        {"alias-rm",      "[group num] [value num]",                                  "Remove a value from an existing alias group (by index in group, see alias-get)", 2, CmdAliasRm<T>},
        {"alias-get",     "",                                                         "Get info on current alias annotations", 0, CmdAliasGet<T>},
        {"reanalyse",     "",                                                         "Re-run all analyses (e.g. after adding annotations)", 0, CmdReanalyse<T>},
        {"exit",          "",                                                         "Exit the debugger", 0, CmdExit<T>},
        {"quit",          "",                                                         "Exit the debugger", 0, CmdExit<T>},
        {"help",          "",                                                         "Show this help text", 0, CmdHelp<T>},
    };
}

template<typename T>
std::string getSourceLine(JumpTraceEntry<T>* trace, bool translate, std::function<std::string(T)> infoToStr) {
    if (translate) {
        if (!trace->loc->getDebugLoc()) return "";
        FileReference ref = ContractPassUtility::getFileReference(trace->loc);
        return std::format("{:>15}:{:0>3} | {}", std::filesystem::path(ref.file).filename().string(), ref.line, TUIManager::getSpecificLine(ref.file, ref.line));
    } else {
        std::string out;
        raw_string_ostream strstream(out);
        trace->loc->print(strstream);
        return out;
    }
}

template<typename T>
JumpTraceEntry<T>* getLinearTrace(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr, int& siblings) {
    JumpTraceEntry<T>* cur_trace = trace;
    siblings = 0;
    do {
        siblings = cur_trace->predecessors.size();
        if (siblings == 1) cur_trace = cur_trace->predecessors[0];
    } while (siblings == 1 && (cur_trace->kind != TraceKind::FUNCEXIT && cur_trace->kind != TraceKind::FUNCENTRY)); // Always show funcentry
    return cur_trace;
}

template<typename T>
std::vector<TraceBlock<T>> GetTraceList(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr, std::map<JumpTraceEntry<T>*,int> preds_select) {
    std::vector<TraceBlock<T>> trace_by_blocks;
    std::map<JumpTraceEntry<T>*,int> trace_to_block;
    JumpTraceEntry<T>* cur_trace = trace;
    int preds = 0;
    do {
        JumpTraceEntry<T>* last_entry = getLinearTrace(traceDB, cur_trace, infoToStr, preds);
        std::string start_loc = ContractPassUtility::getInstrLocStr(cur_trace->loc, false);
        if (start_loc == "UNKNOWN" && cur_trace->loc->getPrevNode()) {
            start_loc = ContractPassUtility::getInstrLocStr(cur_trace->loc->getPrevNode(), false); // Second try
        }
        std::string end_loc;
        if (&*last_entry->loc->getFunction()->getEntryBlock().begin() == last_entry->loc) // Function begin does not have dbg info, but header does
            end_loc = ContractPassUtility::getInstrLocStr(last_entry->loc->getFunction(), false) + " (Function \"" + last_entry->loc->getFunction()->getName().str() + "\" entrypoint)";
        else
            end_loc = ContractPassUtility::getInstrLocStr(last_entry->loc, false);
        std::string full_line;
        if (trace_to_block.contains(cur_trace)) {
            // There is a loop or goto to earlier block
            full_line = std::format("From {} to {} then jump to block {}", start_loc, end_loc, trace_to_block[cur_trace]);
            preds = 0;
        } else {
            // Linear execution
            full_line = std::format("From {} to {}", start_loc, end_loc); // The same for all
            if (preds != 0) full_line += std::format(" then {}", traceKindToStr(last_entry->kind)) + (preds > 1 ? std::format(" [Viewing Child {}/{}]", preds_select[last_entry], preds-1) : "");
        }
        trace_by_blocks.push_back({full_line, cur_trace, last_entry});
        trace_to_block.insert({cur_trace, trace_by_blocks.size()-1});
        if (preds >= 1) {
            if (!preds_select.contains(last_entry)) preds_select[last_entry] = 0;
            cur_trace = last_entry->predecessors[preds_select[last_entry]];
        }
    } while (preds != 0);
    return trace_by_blocks;
}

template<typename T>
std::vector<std::string> getBlockLines(TraceBlock<T> block, bool transToSource, std::function<std::string(T)> infoToStr) {
    std::vector<std::string> lines;
    std::string last;
    for (JumpTraceEntry<T>* cur_trace = block.first_entry; cur_trace != block.last_entry; cur_trace = cur_trace->predecessors[0]) {
        std::string newline = getSourceLine(cur_trace, transToSource, infoToStr);
        if (newline.empty() || newline == last) continue;
        lines.push_back(std::format("{:>{}} | {}", infoToStr(cur_trace->analysisInfo), UI_ANALYSISINFO_PAD_SIZE, newline));
        last = newline;
    }
    // Print last
    std::string newline = getSourceLine(block.last_entry, transToSource, infoToStr);
    if (!newline.empty() && newline != last)
        lines.push_back(std::format("{:>{}} | {}", infoToStr(block.last_entry->analysisInfo), UI_ANALYSISINFO_PAD_SIZE, newline));
    return lines;
}

template<typename T>
void ShowBlock(TraceBlock<T> block, bool transToSource, std::function<std::string(T)> infoToStr) {
    std::vector<std::string> lines = getBlockLines(block, transToSource, infoToStr);
    std::vector<ftxui::Element> elems;
    for (std::string line : lines) elems.push_back(ftxui::text(line));
    std::string title = "Block Preview";
    if (transToSource) title += ", Source Approx.";
    else title += ", LLVM IR";
    TUIManager::ShowLines(elems, title);
}

template<typename T>
bool ShowTrace(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr) {
    std::map<JumpTraceEntry<T>*,int> sibling_select;
    std::map<JumpTraceEntry<T>*,bool> expand_select;
    std::string last_res = "";
    while (true) {
        std::vector<TraceBlock<T>> trace_by_blocks = GetTraceList(traceDB, trace, infoToStr, sibling_select);
        std::vector<ftxui::Element> full_trace;
        for (int i = 0; i < trace_by_blocks.size(); i++) {
            std::string trace_block_str = std::format("{:>3}: {:^{}} - {}", i, infoToStr(trace_by_blocks[i].first_entry->analysisInfo), UI_ANALYSISINFO_PAD_SIZE, trace_by_blocks[i].trace_list);
            full_trace.push_back(ftxui::text(trace_block_str));
            if (expand_select.contains(trace_by_blocks[i].last_entry) && expand_select[trace_by_blocks[i].last_entry]) {
                std::vector<std::string> block_lines = getBlockLines(trace_by_blocks[i], false, infoToStr);
                std::vector<ftxui::Element> formatted_lines;
                for (std::string block_line : block_lines) {
                    formatted_lines.push_back(ftxui::text(std::format("{:>3}.{:0>3}", i, formatted_lines.size()) + block_line));
                }
                full_trace.insert(full_trace.end(), formatted_lines.begin(), formatted_lines.end());
            }
        }

        // Print current module
        const llvm::Module* M = trace->loc->getModule();
        std::error_code rc;
        llvm::raw_fd_stream reanalyse_file("CoVer_reanalyse.ll", rc);
        M->print(reanalyse_file, nullptr);

        std::string input = TUIManager::RenderTxtEntry(full_trace, "JumpTrace", last_res);

        // Get command and verify input sizing
        std::vector<CmdInfo<T>> commands = GetTraceCommands<T>();
        auto used_cmd = std::find_if(commands.begin(), commands.end(), [&](CmdInfo<T> const& cmd){
            return input.starts_with(cmd.name);
        });
        if (used_cmd == commands.end()) {
            last_res = "Unknown command: " + input;
            continue;
        }
        std::vector<std::string> args;
        if (!verifyInputArgs(used_cmd->name, input, args, used_cmd->num_params)) {
            last_res = std::format("Invalid syntax. Usage: {} {}", used_cmd->name, used_cmd->usage);
            continue;
        }

        // Run command
        CmdContext<T> ctx{trace_by_blocks, sibling_select, expand_select, infoToStr};
        CmdResult result = used_cmd->handler(args, ctx, last_res);
        if (result.has_value()) return result.value();
    }
}

} // namespace TUITrace
