#pragma once

#include <cassert>
#include <fstream>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <json/value.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <vector>

#include "../Passes/ContractManager.hpp"
#include "ContractPassUtility.hpp"
#include "ErrorMessage.h"

#define UI_ANALYSISINFO_PAD_SIZE 15

using namespace llvm;

template<typename T>
using JumpTraceEntry = ContractPassUtility::JumpTraceEntry<T>;

template<typename T>
using TraceDB = ContractPassUtility::TraceDB<T>;

using TraceKind = ContractPassUtility::TraceKind;
using Contract = llvm::ContractManagerAnalysis::Contract;

namespace TUIManager {
    void StartMenu(ContractManagerAnalysis::ContractDatabase DB);
    void ShowContractDetails(ContractManagerAnalysis::Contract C);
    void ResultsScreen(Json::Value res, std::map<Json::Value, const Contract> JsonMsgToContr);

    template<typename T>
    void ShowTrace(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr);
    template<typename T>
    struct TraceBlock {
        std::string trace_list;
        JumpTraceEntry<T>* first_entry;
        JumpTraceEntry<T>* last_entry;
    };
    template<typename T>
    JumpTraceEntry<T>* getLinearTrace(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr, int& siblings);
    template<typename T>
    std::vector<TUIManager::TraceBlock<T>> GetTraceList(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr, std::map<JumpTraceEntry<T>*,int> preds_select);

    template<typename T>
    bool DebugMenu(ContractPassUtility::WorklistResult<T> WLRes);

    std::string RenderTxtEntry(std::vector<ftxui::Element> lines, std::string title, std::string last_res);
    int RenderMenu(std::vector<std::string> choices, std::string title);
    void ShowFile(std::string file, std::map<int,ftxui::Color> highlights, int focus_line = -1);
    void ShowLines(std::vector<ftxui::Element> lines);

    template<typename T>
    void ShowBlock(TraceBlock<T> block, bool transToSource, std::function<std::string(T)> infoToStr);

    std::string traceKindToStr(TraceKind kind);
}

template<typename T>
JumpTraceEntry<T>* TUIManager::getLinearTrace(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr, int& siblings) {
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
bool TUIManager::DebugMenu(ContractPassUtility::WorklistResult<T> WLRes) {
    return false;
}

template<typename T>
std::vector<TUIManager::TraceBlock<T>> TUIManager::GetTraceList(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr, std::map<JumpTraceEntry<T>*,int> preds_select) {
    std::vector<TUIManager::TraceBlock<T>> trace_by_blocks;
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
        std::string full_line = traceKindToStr(cur_trace->kind) + " from " + start_loc + " to " + end_loc; // The same for all
        if (preds != 0) full_line += " then " +  traceKindToStr(last_entry->kind) + (preds > 1 ? " [Viewing Child " + std::to_string(preds_select[last_entry]) + "/" + std::to_string(preds - 1) + "]" : "");
        trace_by_blocks.push_back({full_line, cur_trace, last_entry});
        //assert(preds != 1 && "buildTraceList returned #preds not eq 1!");
        if (preds >= 1) {
            if (!preds_select.contains(last_entry)) preds_select[last_entry] = 0;
            cur_trace = last_entry->predecessors[preds_select[last_entry]];
        }
    } while (preds != 0);
    return trace_by_blocks;
}

inline std::string getSpecificLine(std::string file, int line) {
    std::ifstream filestream(file);
    std::string out;
    int cur_line = 0;
    for (int i = 0; i < line; i++) {
        std::getline(filestream, out);
    }
    return out;
}

template<typename T>
std::string getSourceLine(JumpTraceEntry<T>* trace, bool translate, std::function<std::string(T)> infoToStr) {
    if (translate) {
        if (!trace->loc->getDebugLoc()) return "";
        FileReference ref = ContractPassUtility::getFileReference(trace->loc);
        return getSpecificLine(ref.file, ref.line);
    } else {
        std::string out;
        raw_string_ostream strstream(out);
        trace->loc->print(strstream);
        return out;
    }
}

template<typename T>
void TUIManager::ShowBlock(TraceBlock<T> block, bool transToSource, std::function<std::string(T)> infoToStr) {
    std::vector<ftxui::Element> lines;
    std::string last;
    for (JumpTraceEntry<T>* cur_trace = block.first_entry; cur_trace != block.last_entry; cur_trace = cur_trace->predecessors[0]) {
        std::string newline = getSourceLine(cur_trace, transToSource, infoToStr);
        if (newline.empty() || newline == last) continue;
        lines.push_back(ftxui::hbox({ftxui::text(infoToStr(cur_trace->analysisInfo)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, UI_ANALYSISINFO_PAD_SIZE), ftxui::text(" | " + newline)}));
        last = newline;
    }
    // Print last
    std::string newline = getSourceLine(block.last_entry, transToSource, infoToStr);
    if (!newline.empty() && newline != last)
        lines.push_back(ftxui::hbox({ftxui::text(infoToStr(block.last_entry->analysisInfo)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, UI_ANALYSISINFO_PAD_SIZE), ftxui::text(" | " + newline)}));
    ShowLines(lines);
}

template<typename T>
void TUIManager::ShowTrace(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr) {
    std::map<JumpTraceEntry<T>*,int> sibling_select;
    std::string last_res = "";
    while (true) {
        std::vector<TUIManager::TraceBlock<T>> trace_by_blocks = GetTraceList(traceDB, trace, infoToStr, sibling_select);
        std::vector<ftxui::Element> full_trace;
        for (int i = 0; i < trace_by_blocks.size(); i++) {
            ftxui::Element trace_block_str = ftxui::hbox({
                ftxui::text(std::to_string(i) + ": "),
                ftxui::text(infoToStr(trace_by_blocks[i].first_entry->analysisInfo)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, UI_ANALYSISINFO_PAD_SIZE),
                ftxui::text(" -- " + trace_by_blocks[i].trace_list)
            });
            full_trace.push_back(trace_block_str);
        }
        std::string input = RenderTxtEntry(full_trace, "JumpTrace", last_res);
        if (input == "exit" || input == "quit") return;
        else if (input == "help") {
            // Show help
            std::vector<ftxui::Element> lines;
            lines = {
                ftxui::text("Trace debug menu"),
                ftxui::text(""),
                ftxui::text("Commands:"),
                ftxui::text("    child [block num] [child num]"),
                ftxui::text("        Show different predecessor blocks"),
                ftxui::text("    view [source|ir] [block num]"),
                ftxui::text("        Show instructions / approxmiate source and analysis information for block"),
                ftxui::text("    exit"),
                ftxui::text("        Leave debug menu"),
            };
            ShowLines(lines);
        } else if (input.starts_with("jump ")) {

        } else if (input.starts_with("child")) {
            if (input == "child") {
                last_res = "Usage: child [block num] [child num]";
                continue;
            }
            std::string args = input.substr(6); // length of "child "
            int block, child;
            try {
                block = std::stoi(args.substr(0, args.find(" ")));
                args = args.substr(args.find(" "));
                child = std::stoi(args);
            } catch (...) {
                last_res = "Invalid syntax for child select command. Usage: child [block num] [child num]";
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
            if (input == "view") {
                last_res = "Usage: view [source|ir] [block num]";
                continue;
            }
            std::string args = input.substr(5); // length of "view "
            bool showSource = false;
            if (args.starts_with("source")) {
                showSource = true;
                args = args.substr(6); // Length of "source"
            } else if (args.starts_with("ir")) {
                args = args.substr(2); // Length of "ir"
            } else {
                last_res = "Invalid syntax for view command.";
                continue;
            }
            int block;
            try {
                block = std::stoi(args);
            } catch (...) {
                last_res = "Invalid syntax for view command.";
                continue;
            }
            if (block < 0 || block >= trace_by_blocks.size()) {
                last_res = "Invalid block number! Must be between 0 and " + std::to_string(trace_by_blocks.size() - 2);
                continue;
            }
            last_res = "";
            TraceBlock<T> selected_block = trace_by_blocks[block];
            ShowBlock(selected_block, showSource, infoToStr);
        } else {
            last_res = "Unknown command: " + input;
        }
    }
}
