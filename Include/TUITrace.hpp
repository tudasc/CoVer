#pragma once

#include <algorithm>
#include <filesystem>
#include <format>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <string>
#include <vector>
#include "ContractPassUtility.hpp"
#include "TUIManager.hpp"

namespace TUITrace {

template<typename T>
using JumpTraceEntry = ContractPassUtility::JumpTraceEntry<T>;

template<typename T>
using TraceDB = ContractPassUtility::TraceDB<T>;

using TraceKind = ContractPassUtility::TraceKind;

template<typename T>
struct TraceBlock {
    std::string trace_list;
    JumpTraceEntry<T>* first_entry;
    JumpTraceEntry<T>* last_entry;
};

struct CmdInfo {
    std::string_view name;
    std::string_view usage;
    std::string_view helptext;
    int num_params;
};

bool verifyInputArgs(std::string_view const& usage, std::string_view const& input, std::vector<std::string>& args, int const& num_inputs);
std::string traceKindToStr(TraceKind kind);

constexpr auto TraceCommands = std::to_array<CmdInfo>({
    {"collapse",  "[block num]",             "Hide IR for block, making it unavailable for annotation", 1},
    {"expand",    "[block num]",             "Show IR for block, and make available for annotation", 1},
    {"child",     "[block num] [child num]", "Show different predecessor blocks", 2},
    {"view",      "[source|ir] [block num]", "Present a preview of IR or a source code approximation of a block", 2},
    {"fp-target", "[block num] [instr num]", "Annotate possible function pointer target(s)", 2},
    {"reanalyse", "",                        "Re-run all analyses (e.g. after adding annotations)", 0},
    {"exit",      "",                        "Exit the debugger", 0},
    {"quit",      "",                        "Exit the debugger", 0},
    {"help",      "",                        "Show this help text", 0},
});

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
    std::vector<std::string> trace_lines;
    siblings = 0;
    do {
        siblings = cur_trace->predecessors.size();
        if (siblings == 1) cur_trace = cur_trace->predecessors[0];
    } while  (siblings == 1 && cur_trace->kind != TraceKind::FUNCEXIT && cur_trace->kind != TraceKind::FUNCENTRY); // Always show funcentry
    return cur_trace;
}

template<typename T>
std::vector<TraceBlock<T>> GetTraceList(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr, std::map<JumpTraceEntry<T>*,int> preds_select) {
    std::vector<TraceBlock<T>> trace_by_blocks;
    JumpTraceEntry<T>* cur_trace = trace;
    int preds = 0;
    do {
        JumpTraceEntry<T>* last_entry = getLinearTrace(traceDB, cur_trace, infoToStr, preds);
        std::string start_loc = ContractPassUtility::getInstrLocStr(cur_trace->loc, false);
        std::string end_loc;
        if (&*last_entry->loc->getParent()->getParent()->getEntryBlock().begin() == last_entry->loc) // Function begin does not have dbg info, but header does
            end_loc = ContractPassUtility::getInstrLocStr(last_entry->loc->getParent()->getParent(), false) + " (Function \"" + last_entry->loc->getParent()->getParent()->getName().str() + "\" entrypoint)";
        else
            end_loc = ContractPassUtility::getInstrLocStr(last_entry->loc, false);
        std::string full_line = std::format("From {} to {}", start_loc, end_loc); // The same for all
        if (preds != 0) full_line += std::format(" then {}", traceKindToStr(last_entry->kind)) + (preds > 1 ? std::format(" [Viewing Child {}/{}]", preds_select[last_entry], preds-1) : "");
        trace_by_blocks.push_back({full_line, cur_trace, last_entry});
        //assert(preds != 1 && "buildTraceList returned #preds not eq 1!");
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
    for (std::string line : lines) {
        elems.push_back(ftxui::text(line));
    }

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
        std::string input = TUIManager::RenderTxtEntry(full_trace, "JumpTrace", last_res);

        // Verify input
        auto used_cmd = std::find_if(TraceCommands.begin(), TraceCommands.end(), [&](CmdInfo const& cmd){
            return input.starts_with(cmd.name);
        });
        if (used_cmd == TraceCommands.end()) {
            last_res = "Unknown command: " + input;
            continue;
        }
        std::vector<std::string> args;
        if (!verifyInputArgs(used_cmd->name, input, args, used_cmd->num_params)) {
            last_res = std::format("Invalid syntax. Usage: {} {}", used_cmd->name, used_cmd->usage);
            continue;
        }

        // Execute command
        if (input == "exit" || input == "quit") return false;
        if (input == "reanalyse") {
            const llvm::Module* M = trace->loc->getModule();
            std::error_code rc;
            llvm::raw_fd_stream reanalyse_file("CoVer_reanalyse.ll", rc);
            M->print(reanalyse_file, nullptr);
            return true;
        }
        else if (input == "help") {
            // Show help
            std::vector<ftxui::Element> lines;
            lines = {
                ftxui::text("Trace debug menu"),
                ftxui::text(""),
                ftxui::text("Commands:"),
            };
            for (CmdInfo const& cmd : TraceCommands) {
                lines.push_back(ftxui::text(std::format("    {} {}", cmd.name, cmd.usage)));
                lines.push_back(ftxui::text(std::format("        {}", cmd.helptext)));
            }
            TUIManager::ShowLines(lines, "Worklist Trace Help Menu");
        } else if (input.starts_with("child")) {
            int block, child;
            try {
                block = std::stoi(args[0]);
                child = std::stoi(args[1]);
            } catch (...) {
                last_res = "Argument(s) are not numbers!";
                continue;
            }
            if (block < 0 || block >= trace_by_blocks.size() - 1) {
                last_res = "Invalid block number! Must be between 0 and " + std::to_string(trace_by_blocks.size() - 2);
                continue;
            }
            TraceBlock<T> selected_block = trace_by_blocks[block];
            if (child < 0 || child >= selected_block.last_entry->predecessors.size()) { // Exclude last, because that one does not have preds
                last_res = "Invalid child number! Must be between 0 and " + std::to_string(selected_block.last_entry->predecessors.size() - 1);
                continue;
            }
            last_res = "Switching child view for block " + std::to_string(block) + " to child " + std::to_string(child);
            sibling_select[selected_block.last_entry] = child;
        } else if (input.starts_with("view")) {
            if (args[0] != "source" && args[0] != "ir") {
                last_res = "Unknown source representation: " + args[0];
                continue;
            }
            int block;
            try {
                block = std::stoi(args[1]);
            } catch (...) {
                last_res = "Second Argument not a number!";
                continue;
            }
            if (block < 0 || block >= trace_by_blocks.size()) {
                last_res = "Invalid block number! Must be between 0 and " + std::to_string(trace_by_blocks.size() - 1);
                continue;
            }
            last_res = "";
            TraceBlock<T> selected_block = trace_by_blocks[block];
            ShowBlock(selected_block, args[0] == "source", infoToStr);
        } else if (input.starts_with("expand")) {
            int block;
            try {
                block = std::stoi(args[0]);
            } catch (...) {
                last_res = "Argument is not a number!";
                continue;
            }
            if (block < 0 || block >= trace_by_blocks.size()) {
                last_res = "Invalid block number! Must be between 0 and " + std::to_string(trace_by_blocks.size() - 1);
                continue;
            }
            last_res = "Expanding block " + std::to_string(block);
            expand_select[trace_by_blocks[block].last_entry] = true;
        } else if (input.starts_with("collapse")) {
            int block;
            try {
                block = std::stoi(args[0]);
            } catch (...) {
                last_res = "Argument is not a number!";
                continue;
            }
            if (block < 0 || block >= trace_by_blocks.size()) {
                last_res = "Invalid block number! Must be between 0 and " + std::to_string(trace_by_blocks.size() - 1);
                continue;
            }
            last_res = "Collapsing block " + std::to_string(block);
            expand_select[trace_by_blocks[block].last_entry] = false;
        } else if (input.starts_with("fp-target")) {
            int block, instr;
            try {
                block = std::stoi(args[0]);
                instr = std::stoi(args[1]);
            } catch (...) {
                last_res = "Argument(s) are not numbers!";
                continue;
            }
            if (block < 0 || block >= trace_by_blocks.size()) {
                last_res = "Invalid block number! Must be between 0 and " + std::to_string(trace_by_blocks.size() - 1);
                continue;
            }
            std::vector<ftxui::Element> elem;
            int count = 0;
            JumpTraceEntry<T>* selected_trace = nullptr;
            JumpTraceEntry<T>* cur_trace = trace_by_blocks[block].first_entry;
            for (JumpTraceEntry<T>* cur_trace = trace_by_blocks[block].first_entry;; cur_trace = cur_trace->predecessors[0]) {
                if (count == instr) {
                    selected_trace = cur_trace;
                    break;
                }
                if (cur_trace == trace_by_blocks[block].last_entry) break;
                count++;
            }
            if (selected_trace == nullptr) {
                last_res = "No such instruction!";
                continue;
            }
            if (!isa<CallBase>(selected_trace->loc) || !dyn_cast<CallBase>(selected_trace->loc)->isIndirectCall()) {
                last_res = "Instruction is not an indirect function call!";
                continue;
            }
            std::vector<std::string> funcs;
            Module* M = selected_trace->loc->getModule();
            for (Function const& F : M->functions()) {
                std::string funcname = F.getName().str();
                if (funcname.starts_with("CoVer_")) continue;
                funcs.push_back(funcname);
            }
            std::string selected_func = funcs[TUIManager::RenderMenu(funcs, "Select Function Target")];
            selected_trace->loc->addAnnotationMetadata(std::format("CoVer_AnnotFP|{}", selected_func));
            last_res = std::format("Added possible target \"{}\" to {}.{}", selected_func, block, instr);
        } else {
            last_res = "Unknown command: " + input;
        }
    }
}

}
