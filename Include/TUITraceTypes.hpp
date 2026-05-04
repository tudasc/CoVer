#pragma once

#include <algorithm>
#include <filesystem>
#include <format>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <map>
#include <optional>
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

template<typename T>
struct CmdContext {
    std::vector<TraceBlock<T>>& trace_by_blocks;
    std::map<JumpTraceEntry<T>*,int>& sibling_select;
    std::map<JumpTraceEntry<T>*,bool>& expand_select;
    std::function<std::string(T)> infoToStr;
};

// nullopt = continue loop, true = reanalyse, false = exit
using CmdResult = std::optional<bool>;

template<typename T>
struct CmdInfo {
    std::string_view name;
    std::string_view usage;
    std::string_view helptext;
    int num_params;
    std::function<CmdResult(std::vector<std::string>&, CmdContext<T>&, std::string&)> handler;
};

bool verifyInputArgs(std::string_view const& usage, std::string_view const& input, std::vector<std::string>& args, int const& num_inputs);
std::string traceKindToStr(TraceKind kind);

} // namespace TUITrace
