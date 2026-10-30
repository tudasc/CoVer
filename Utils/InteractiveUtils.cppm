module;

#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ContractTree.hpp"

export module InteractiveUtils;
import LLVMModule;
import ErrorMessage;
import ContractPassUtility;

using namespace llvm;

// Frontend independent parts of the interactive mode, shared by the TUI and GUI. Trace commands are in InteractiveCmds.
namespace InteractiveUtils {

using ContractPassUtility::JumpTraceEntry;
using ContractPassUtility::TraceDB;
using ContractPassUtility::TraceKind;

export constexpr int UI_ANALYSISINFO_PAD_SIZE = 15;

export std::string getSpecificLine(std::string file, int line) {
    std::ifstream filestream(file);
    std::string out;
    int cur_line = 0;
    for (int i = 0; i < line; i++) {
        std::getline(filestream, out);
    }
    return out;
}

// Duration in seconds, as milliseconds below one second
export std::string formatDuration(double seconds) {
    return seconds < 1 ? std::format("{:.1f}ms", seconds * 1000) : std::format("{:.2f}s", seconds);
}

export template<typename T>
struct TraceBlock {
    std::string trace_list;
    JumpTraceEntry<T>* first_entry;
    JumpTraceEntry<T>* last_entry;
    std::string start_loc;
    std::string end_loc;
    std::optional<int> jump_to; // Set if the block is a repetition of an earlier block (loop or goto)
};

export std::string traceKindToStr(ContractPassUtility::TraceKind kind) {
    switch (kind) {
        case ContractPassUtility::TraceKind::LINEAR: return "LINEAR";
        case ContractPassUtility::TraceKind::BRANCH: return "BRANCH";
        case ContractPassUtility::TraceKind::SWITCH: return "SWITCH";
        case ContractPassUtility::TraceKind::FUNCENTRY: return "FUNCENTRY";
        case ContractPassUtility::TraceKind::FUNCEXIT: return "FUNCEXIT";
    }
}

export template<typename T>
std::string getSourceLine(JumpTraceEntry<T>* trace, bool translate, std::function<std::string(T)> infoToStr) {
    if (translate) {
        if (!trace->loc->getDebugLoc()) return "";
        FileReference ref = ContractPassUtility::getFileReference(trace->loc);
        return std::format("{:>15}:{:0>3} | {}", std::filesystem::path(ref.file).filename().string(), ref.line, getSpecificLine(ref.file, ref.line));
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

export template<typename T>
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
        std::optional<int> jump_to;
        if (trace_to_block.contains(cur_trace)) {
            // There is a loop or goto to earlier block
            full_line = std::format("From {} to {} then jump to block {}", start_loc, end_loc, trace_to_block[cur_trace]);
            jump_to = trace_to_block[cur_trace];
            preds = 0;
        } else {
            // Linear execution
            full_line = std::format("From {} to {}", start_loc, end_loc); // The same for all
            if (preds != 0) full_line += std::format(" then {}", traceKindToStr(last_entry->kind)) + (preds > 1 ? std::format(" [Viewing Child {}/{}]", preds_select[last_entry], preds-1) : "");
        }
        trace_by_blocks.push_back({full_line, cur_trace, last_entry, start_loc, end_loc, jump_to});
        trace_to_block.insert({cur_trace, trace_by_blocks.size()-1});
        if (preds >= 1) {
            if (!preds_select.contains(last_entry)) preds_select[last_entry] = 0;
            cur_trace = last_entry->predecessors[preds_select[last_entry]];
        }
    } while (preds != 0);
    return trace_by_blocks;
}

export template<typename T>
std::vector<JumpTraceEntry<T>*> getBlockEntries(TraceBlock<T> const& block) {
    std::vector<JumpTraceEntry<T>*> entries = {block.first_entry};
    while (entries.back() != block.last_entry) entries.push_back(entries.back()->predecessors[0]);
    return entries;
}

export struct BlockLine {
    int instr; // Index of the instruction, or index of first if aggregate due to source approx
    std::string text;
};

export template<typename T>
std::vector<BlockLine> getBlockLines(TraceBlock<T> const& block, bool transToSource, std::function<std::string(T)> const& infoToStr) {
    std::vector<BlockLine> lines;
    std::string last;
    for (auto const [instr, entry] : std::views::enumerate(getBlockEntries(block))) {
        std::string line = getSourceLine(entry, transToSource, infoToStr);
        if (line.empty() || line == last) continue;
        lines.emplace_back(instr, std::format("{:>{}} | {}", infoToStr(entry->analysisInfo), UI_ANALYSISINFO_PAD_SIZE, line));
        last = std::move(line);
    }
    return lines;
}

// Leaf formula whose worklist analysis trace can be inspected
export bool isDebuggable(ContractTree::ContractFormula const& form) {
    if (!form.Children.empty()) return false;
    return static_cast<ContractTree::ContractExpression const&>(form).WorklistInfo != nullptr;
}

// Print current module, used as input for reanalysis and diffing
export void WriteReanalyseModule(const llvm::Module* M) {
    std::error_code rc;
    llvm::raw_fd_stream reanalyse_file("CoVer_Reanalyse.ll", rc);
    M->print(reanalyse_file, nullptr);
}

} // namespace InteractiveUtils
