#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

import ContractPassUtility;

namespace TUITrace {

using ContractPassUtility::JumpTraceEntry;
using ContractPassUtility::TraceDB;
using ContractPassUtility::TraceKind;

template<typename T>
struct TraceBlock {
    std::string trace_list;
    JumpTraceEntry<T>* first_entry;
    JumpTraceEntry<T>* last_entry;
    std::string start_loc;
    std::string end_loc;
    std::optional<int> jump_to; // Set if the block is a repetition of an earlier block (loop or goto)
};

enum struct CmdResultCode {
    UNKNOWN_COMMAND,
    INVALID_SYNTAX,
    INVALID_INPUT,
    SUCCESS_CONTINUE,
    SUCCESS_REANALYSE,
    SUCCESS_EXIT
};
struct CmdResult {
    CmdResultCode code;
    std::string res;
};

// Display hooks for commands that present something to the user, implemented by the TUI and GUI respectively
struct CmdFrontend {
    std::function<void(std::vector<std::string> const& lines, std::string const& title)> showLines;
    std::function<std::vector<std::string>(std::vector<std::string> const& choices, std::string const& title, std::set<std::string> const& preselected)> selectMultiple;
    std::function<void(std::vector<std::pair<std::string, CmdResult>> const& history)> showHistory;
};

template<typename T>
struct CmdContext {
    std::vector<TraceBlock<T>>& trace_by_blocks;
    std::map<JumpTraceEntry<T>*,int>& sibling_select;
    std::map<JumpTraceEntry<T>*,bool>& expand_select;
    std::vector<std::pair<std::string, CmdResult>>& history;
    std::function<std::string(T)> infoToStr;
    CmdFrontend frontend;
};

template<typename T>
struct CmdInfo {
    std::string_view name;
    std::string_view usage;
    std::string_view helptext;
    int num_params;
    std::function<CmdResult(std::vector<std::string>&, CmdContext<T>&)> handler;
};

} // namespace TUITrace
