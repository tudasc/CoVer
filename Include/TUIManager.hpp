
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <json/value.h>
#include <string>
#include <vector>

#include "../Passes/ContractManager.hpp"
#include "ContractPassUtility.hpp"

using namespace llvm;

template<typename T>
using JumpTraceEntry = ContractPassUtility::JumpTraceEntry<T>;

using TraceKind = ContractPassUtility::TraceKind;

namespace TUIManager {
    void StartMenu(ContractManagerAnalysis::ContractDatabase DB);
    void ShowContractDetails(ContractManagerAnalysis::Contract C);
    void ResultsScreen(Json::Value res);

    template<typename T>
    void ShowTrace(std::map<const Instruction *, JumpTraceEntry<T>> traceDB, JumpTraceEntry<T> trace, std::function<std::string(T)> infoToStr);
    template<typename T>
    struct TraceBlock {
        std::vector<std::string> trace_list;
        JumpTraceEntry<T> last_entry;
    };
    template<typename T>
    TraceBlock<T> buildTraceList(std::map<const Instruction *, JumpTraceEntry<T>> traceDB, JumpTraceEntry<T> trace, std::function<std::string(T)> infoToStr, int& siblings);

    int RenderMenu(std::vector<std::string> choices, std::string title);

    std::string traceKindToStr(TraceKind kind);
}

template<typename T>
TUIManager::TraceBlock<T> TUIManager::buildTraceList(std::map<const Instruction *, JumpTraceEntry<T>> traceDB, JumpTraceEntry<T> trace, std::function<std::string(T)> infoToStr, int& siblings) {
    std::vector<std::string> trace_lines;
    siblings = 0;
    do {
        std::string out;
        raw_string_ostream line(out);
        trace.loc->print(line);
        trace_lines.push_back(traceKindToStr(trace.kind) + " | " + infoToStr(trace.analysisInfo) + " | " + line.str());
        if (trace.kind != TraceKind::LINEAR) {
            siblings = traceDB[trace.loc].predecessors.size();
        } else {
            siblings = trace.predecessors.size();
        }
        if (siblings == 1) trace = trace.predecessors[0];
    } while  (siblings == 1);
    return {trace_lines, trace};
}

template<typename T>
void TUIManager::ShowTrace(std::map<const Instruction *, JumpTraceEntry<T>> traceDB, JumpTraceEntry<T> trace, std::function<std::string(T)> infoToStr) {
    std::vector<TraceBlock<T>> trace_by_blocks;
    JumpTraceEntry<T> cur_trace = trace;
    int siblings = 0;
    do {
        TraceBlock<T> block = buildTraceList(traceDB, cur_trace, infoToStr, siblings);
        cur_trace = block.last_entry;
        trace_by_blocks.push_back({block.trace_list, cur_trace});
        if (siblings > 0) {
            if (siblings > 1) trace_by_blocks.push_back({{"-- Next Block has " + std::to_string(siblings) + " siblings. Select to view --"}, cur_trace});
            cur_trace = cur_trace.predecessors[0];
        }
    } while (siblings != 0);
    
    std::vector<std::string> full_trace;
    full_trace.push_back("Exit Trace");
    for (TraceBlock<T> trace_block : trace_by_blocks) {
        std::vector<std::string> trace_block_str = trace_block.trace_list;
        full_trace.insert(full_trace.end(), trace_block_str.begin(), trace_block_str.end());
    }

    int choice = RenderMenu(full_trace, "JumpTrace");
    if (choice == 0) return;

    // Resolve selected entry
    choice--; // "Exit Trace" entry
    bool found = false;
    TraceBlock<T> selected_entry;
    for (TraceBlock<T> trace_block : trace_by_blocks) {
        for (std::string entry : trace_block.trace_list) {
            if (choice == 0) {
                found = true;
                selected_entry = trace_block;
                break;
            }
            choice--;
        }
        if (found) break;
    }
    if (traceDB[selected_entry.last_entry.loc].predecessors.size() > 1) {
        ShowTrace(traceDB, traceDB[selected_entry.last_entry.loc].predecessors[1], infoToStr);
    }
}
