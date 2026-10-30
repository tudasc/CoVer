module;

#include <algorithm>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module InteractiveCmds;
import LLVMModule;
import ContractPassUtility;
import IRPatch;
import InteractiveUtils;

using namespace llvm;

// Trace debugger commands, shared by the TUI and GUI
namespace InteractiveCmds {

using ContractPassUtility::JumpTraceEntry;
using InteractiveUtils::TraceBlock;

export enum struct CmdResultCode {
    UNKNOWN_COMMAND,
    INVALID_SYNTAX,
    INVALID_INPUT,
    SUCCESS_CONTINUE,
    SUCCESS_REANALYSE,
    SUCCESS_EXIT
};
export struct CmdResult {
    CmdResultCode code;
    std::string res;
};

// Display hooks for commands that present something to the user, implemented by the TUI and GUI respectively
export struct CmdFrontend {
    std::function<void(std::vector<std::string> const& lines, std::string const& title)> showLines;
    std::function<std::vector<std::string>(std::vector<std::string> const& choices, std::string const& title, std::set<std::string> const& preselected)> selectMultiple;
    std::function<void(std::vector<std::pair<std::string, CmdResult>> const& history)> showHistory;
};

export template<typename T>
struct CmdContext {
    std::vector<TraceBlock<T>>& trace_by_blocks;
    std::map<JumpTraceEntry<T>*,int>& sibling_select;
    std::map<JumpTraceEntry<T>*,bool>& expand_select;
    bool& expand_as_source;
    std::vector<std::pair<std::string, CmdResult>>& history;
    std::function<std::string(T)> infoToStr;
    CmdFrontend frontend;
};

export template<typename T>
struct CmdInfo {
    std::string_view name;
    std::string_view usage;
    std::string_view helptext;
    int num_params;
    std::function<CmdResult(std::vector<std::string>&, CmdContext<T>&)> handler;
};

// Command history of a previous run, if this is a reanalysis
export std::vector<std::pair<std::string, CmdResult>> LoadHistoryCache() {
    std::vector<std::pair<std::string, CmdResult>> input_history;
    if (std::filesystem::exists("CoVer_HistoryCache")) {
        std::string line;
        std::ifstream historyfile("CoVer_HistoryCache");
        std::getline(historyfile, line);
        while (std::getline(historyfile, line)) {
            if (line == "|| DIFF HISTORY END ||") break;
            CmdResultCode code = (CmdResultCode)std::atoi(line.substr(0, line.find_first_of("|")).c_str());
            input_history.push_back({line.substr(line.find_first_of("|") + 1), {code, ""}});
        }
        std::filesystem::remove("CoVer_HistoryCache");
    }
    return input_history;
}

export void SaveHistoryCache(std::vector<std::pair<std::string, CmdResult>> const& history) {
    std::ofstream difffile("CoVer_HistoryCache");
    difffile << "|| DIFF HISTORY BEGIN ||\n";
    for (std::pair<std::string, CmdResult> const& x : history) {
        difffile << (int)x.second.code << "|" << x.first << "\n";
    }
    difffile << "|| DIFF HISTORY END ||\n---\n";
}

template<typename T>
extern std::vector<CmdInfo<T>> TraceCommands;

// Parse a whole argument as integer, the error names the argument
std::expected<int, std::string> ParseInt(std::string_view s, std::string_view arg_name) {
    int value;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::unexpected(std::format("{} argument is not a number!", arg_name));
    return value;
}

template<typename T>
std::expected<int, std::string> ParseBlock(std::string_view s, CmdContext<T> const& ctx) {
    std::expected<int, std::string> block = ParseInt(s, "Block");
    if (block && (*block < 0 || *block >= ctx.trace_by_blocks.size()))
        return std::unexpected(std::format("Invalid block number! Must be between 0 and {}", ctx.trace_by_blocks.size() - 1));
    return block;
}

template<typename T>
std::expected<Instruction*, std::string> ParseTraceInstr(std::string_view s, int block, CmdContext<T> const& ctx) {
    std::expected<int, std::string> instr = ParseInt(s, "Instruction");
    if (!instr) return std::unexpected(instr.error());
    std::vector<JumpTraceEntry<T>*> entries = InteractiveUtils::getBlockEntries(ctx.trace_by_blocks[block]);
    if (*instr < 0 || *instr >= entries.size()) return std::unexpected("No such instruction!");
    return entries[*instr]->loc;
}

inline std::expected<int, std::string> ParseAliasGroup(std::string_view s) {
    std::expected<int, std::string> group = ParseInt(s, "Group");
    if (group && !ContractPassUtility::getAliasAnnots().contains(*group))
        return std::unexpected("Invalid alias group! Check existing groups using alias-get");
    return group;
}

template<typename T>
CmdResult CmdCollapse(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::expected<int, std::string> block = ParseBlock(args[0], ctx);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    ctx.expand_select[ctx.trace_by_blocks[*block].last_entry] = false;
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Collapsing block {}", *block)};
}

template<typename T>
CmdResult CmdExpand(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::expected<int, std::string> block = ParseBlock(args[0], ctx);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    ctx.expand_select[ctx.trace_by_blocks[*block].last_entry] = true;
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Expanding block {}", *block)};
}

template<typename T>
CmdResult CmdChild(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::expected<int, std::string> block = ParseBlock(args[0], ctx);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    std::expected<int, std::string> child = ParseInt(args[1], "Child");
    if (!child) return {CmdResultCode::INVALID_INPUT, child.error()};
    JumpTraceEntry<T>* last_entry = ctx.trace_by_blocks[*block].last_entry;
    if (*child < 0 || *child >= last_entry->predecessors.size()) {
        return {CmdResultCode::INVALID_INPUT, std::format("Invalid child number! Must be between 0 and {}", last_entry->predecessors.size() - 1)};
    }
    ctx.sibling_select[last_entry] = *child;
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Switching child view for block {} to child {}", *block, *child)};
}

template<typename T>
CmdResult CmdShow(std::vector<std::string>& args, CmdContext<T>& ctx) {
    if (args[0] != "source" && args[0] != "ir") {
        return {CmdResultCode::INVALID_INPUT, "Unknown source representation: " + args[0]};
    }
    std::expected<int, std::string> block = ParseBlock(args[1], ctx);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    bool transToSource = args[0] == "source";
    std::vector<std::string> lines = InteractiveUtils::getBlockLines(ctx.trace_by_blocks[*block], transToSource, ctx.infoToStr)
                                   | std::views::transform(&InteractiveUtils::BlockLine::text) | std::ranges::to<std::vector>();
    ctx.frontend.showLines(lines, std::format("Block Preview, {}", transToSource ? "Source Approx." : "LLVM IR"));
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

template<typename T>
CmdResult CmdToggleView(std::vector<std::string>&, CmdContext<T>& ctx) {
    ctx.expand_as_source = !ctx.expand_as_source;
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Expanded blocks are now shown as {}", ctx.expand_as_source ? "source code approximation" : "LLVM IR")};
}

template<typename T>
CmdResult CmdFpTarget(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::expected<int, std::string> block = ParseBlock(args[0], ctx);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    std::expected<Instruction*, std::string> selected = ParseTraceInstr(args[1], *block, ctx);
    if (!selected) return {CmdResultCode::INVALID_INPUT, selected.error()};
    CallBase* CB = dyn_cast<CallBase>(*selected);
    if (!CB || !CB->isIndirectCall()) {
        return {CmdResultCode::INVALID_INPUT, "Instruction is not an indirect function call!"};
    }
    std::vector<std::string> funcs;
    Module* M = CB->getModule();
    for (Function const& F : M->functions()) {
        if (!F.getName().starts_with("CoVer_") && !F.getName().starts_with("llvm.")) funcs.push_back(F.getName().str());
    }
    std::set<std::string> previously_selected = ContractPassUtility::getFPAnnots(CB)
                                              | std::views::transform([](Function* F) { return F->getName().str(); }) | std::ranges::to<std::set>();
    std::set<Function*> selected_func = ctx.frontend.selectMultiple(funcs, "Select Function Target", previously_selected)
                                      | std::views::transform([&](std::string const& name) { return M->getFunction(name); }) | std::ranges::to<std::set>();
    ContractPassUtility::setFPTarget(CB, selected_func);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("{}.{} now has {} possible target(s)", *block, args[1], selected_func.size())};
}

template<typename T>
CmdResult CmdAliasCreate(std::vector<std::string>& args, CmdContext<T>& ctx) {
    if (args[0] != "yes" && args[0] != "y" && args[0] != "no" && args[0] != "n") {
        return {CmdResultCode::INVALID_INPUT, "Unknown alias type " + args[0]};
    }
    bool isAlias = args[0].starts_with("y");
    std::expected<int, std::string> block1 = ParseBlock(args[1], ctx);
    if (!block1) return {CmdResultCode::INVALID_INPUT, block1.error()};
    std::expected<int, std::string> block2 = ParseBlock(args[3], ctx);
    if (!block2) return {CmdResultCode::INVALID_INPUT, block2.error()};
    Value* V1 = ContractPassUtility::getValueByName(args[2], ctx.trace_by_blocks[*block1].first_entry->loc->getFunction());
    Value* V2 = ContractPassUtility::getValueByName(args[4], ctx.trace_by_blocks[*block2].first_entry->loc->getFunction());
    if (!V1 || !V2) { return {CmdResultCode::INVALID_INPUT, "Could not identify value " + (!V1 ? args[2] : args[4])}; }
    ContractPassUtility::createAliasGroup(isAlias, V1, V2);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Values {} and {} {} alias", V1->getNameOrAsOperand(), V2->getNameOrAsOperand(), isAlias ? "should" : "should not")};
}

template<typename T>
CmdResult CmdAliasAdd(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::expected<int, std::string> group = ParseAliasGroup(args[0]);
    if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
    std::expected<int, std::string> block = ParseBlock(args[1], ctx);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    Value* V = ContractPassUtility::getValueByName(args[2], ctx.trace_by_blocks[*block].first_entry->loc->getFunction());
    if (!V) { return {CmdResultCode::INVALID_INPUT, "Could not identify value " + args[2]}; }
    ContractPassUtility::addToAliasGroup(*group, V);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Values {} added to alias group {}", V->getNameOrAsOperand(), *group)};
}

template<typename T>
CmdResult CmdAliasType(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::expected<int, std::string> group = ParseAliasGroup(args[0]);
    if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
    bool isAlias = args[1].starts_with("y");
    ContractPassUtility::setAliasGroupType(*group, isAlias);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Alias group {} {}", *group, isAlias ? "should alias" : "should not alias")};
}

template<typename T>
CmdResult CmdAliasRm(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::expected<int, std::string> group = ParseAliasGroup(args[0]);
    if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
    if (args[1].starts_with("e")) {
        std::expected<int, std::string> idx = ParseInt(std::string_view(args[1]).substr(1), "Epoch index");
        if (!idx) return {CmdResultCode::INVALID_INPUT, idx.error()};
        int num_epochs = ContractPassUtility::getAliasEpochs(*group).size();
        if (*idx < 0 || *idx >= num_epochs) {
            return {CmdResultCode::INVALID_INPUT, std::format("Epoch index must be between e0 and e{}", num_epochs - 1)};
        }
        ContractPassUtility::removeAliasEpoch(*group, *idx);
        return {CmdResultCode::SUCCESS_CONTINUE, std::format("Removed alias epoch {:>3}.e{:0>2}", *group, *idx)};
    }
    std::expected<int, std::string> idx = ParseInt(args[1], "Group member index");
    if (!idx) return {CmdResultCode::INVALID_INPUT, idx.error()};
    int num_members = ContractPassUtility::getAliasAnnots().at(*group).members.size();
    if (*idx < 0 || *idx >= num_members) {
        return {CmdResultCode::INVALID_INPUT, std::format("Group member index must be between 0 and {}", num_members - 1)};
    }
    ContractPassUtility::removeFromAliasGroup(*group, *idx);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Removed alias annotation {:>3}.{:0>3}", *group, *idx)};
}

template<typename T>
CmdResult CmdAliasEpoch(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::expected<int, std::string> group = ParseAliasGroup(args[0]);
    if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
    std::expected<int, std::string> block = ParseBlock(args[1], ctx);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    std::expected<Instruction*, std::string> selected = ParseTraceInstr(args[2], *block, ctx);
    if (!selected) return {CmdResultCode::INVALID_INPUT, selected.error()};
    ContractPassUtility::addAliasEpoch(*group, *selected);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Added epoch for alias group {} at {}.{}", *group, *block, args[2])};
}

template<typename T>
CmdResult CmdAliasGet(std::vector<std::string>&, CmdContext<T>& ctx) {
    std::map<int, ContractPassUtility::AliasGroup> const AGs = ContractPassUtility::getAliasAnnots();
    std::vector<std::string> group_lines;
    for (std::pair<int, ContractPassUtility::AliasGroup> const& G : AGs) {
        group_lines.push_back(std::format("Group {}, {}", G.first, G.second.areAliasing ? "should alias" : "should not alias"));
        int num = 0;
        for (Value const* V : G.second.members) {
            std::string out;
            raw_string_ostream strstream(out);
            V->print(strstream);
            group_lines.push_back(std::format("{}.{:0>3} | {}", G.first, num++, out));
        }
        // Epoch nums
        num = 0;
        for (CallBase const* CB : ContractPassUtility::getAliasEpochs(G.first)) {
            group_lines.push_back(std::format("{}.e{:0>2} | epoch in {} at {}", G.first, num++, CB->getFunction()->getName().str(), ContractPassUtility::getInstrLocStr(CB)));
        }
    }
    ctx.frontend.showLines(group_lines, "Current Alias Groups");
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

template<typename T>
CmdResult CmdAllocCreate(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::expected<int, std::string> block = ParseBlock(args[0], ctx);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    std::expected<Instruction*, std::string> selected = ParseTraceInstr(args[1], *block, ctx);
    if (!selected) return {CmdResultCode::INVALID_INPUT, selected.error()};
    if (args[2].starts_with("-")) return {CmdResultCode::INVALID_INPUT, "Size argument must not be negative!"};
    std::expected<uint64_t, std::string> size = ParseInt(args[2], "Size");
    if (!size) return {CmdResultCode::INVALID_INPUT, size.error()};
    std::string err = ContractPassUtility::setAllocAnnot(*selected, *size);
    if (!err.empty()) return {CmdResultCode::INVALID_INPUT, err};
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("{} at {}.{} is now annotated as allocated{}", (*selected)->getNameOrAsOperand(), *block, args[1], *size ? std::format(" with at least {} bytes", *size) : "")};
}

template<typename T>
CmdResult CmdDiff(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::ofstream difffile(args[0]);
    difffile << "|| DIFF HISTORY BEGIN ||\n";
    for (auto const& [command, result] : ctx.history) {
        difffile << command << "\n";
    }
    difffile << "|| DIFF HISTORY END ||\n---\n";
    int rc = std::system("diff -c -I '^; ModuleID' -I '^!.*' -I '^@.*' CoVer_InteractStart.ll CoVer_Reanalyse.ll > CoVer_Diff.tmp");
    if (!WIFEXITED(rc) || (WEXITSTATUS(rc) && WEXITSTATUS(rc) != 1)) {
        return {CmdResultCode::INVALID_INPUT, std::format("Error, diff returned exit code {}", WEXITSTATUS(rc))};
    }
    difffile << std::ifstream("CoVer_Diff.tmp").rdbuf();
    return {CmdResultCode::SUCCESS_CONTINUE, "Wrote patch file to " + args[0]};
}

template<typename T>
CmdResult CmdPatch(std::vector<std::string>& args, CmdContext<T>& ctx) {
    if (!std::filesystem::exists(args[0])) {
        return {CmdResultCode::INVALID_INPUT, "File does not exist!"};
    }
    std::string err = IRPatch::applyPatch("CoVer_Reanalyse.ll", args[0]);
    if (!err.empty()) return {CmdResultCode::INVALID_INPUT, "Error, " + err};
    return {CmdResultCode::SUCCESS_REANALYSE, "Applied patch from " + args[0]};
}

template<typename T>
CmdResult CmdDump(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::error_code EC;
    raw_fd_ostream out(args[0], EC);
    Module* M = ctx.trace_by_blocks.begin()->first_entry->loc->getModule();
    M->print(out, nullptr);
    out << "\n";
    return {CmdResultCode::SUCCESS_CONTINUE, "Wrote module IR to " + args[0]};
}

template<typename T>
CmdResult CmdHistory(std::vector<std::string>&, CmdContext<T>& ctx) {
    ctx.frontend.showHistory(ctx.history);
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

template<typename T>
CmdResult CmdReanalyse(std::vector<std::string>&, CmdContext<T>&) {
    return {CmdResultCode::SUCCESS_REANALYSE, "Re-running analysis..."};
}

template<typename T>
CmdResult CmdExit(std::vector<std::string>&, CmdContext<T>&) {
    return {CmdResultCode::SUCCESS_EXIT, "Exiting debugger..."};
}

template<typename T>
CmdResult CmdHelp(std::vector<std::string>&, CmdContext<T>& ctx) {
    std::vector<std::string> lines = {
        "Trace debug menu",
        "",
        "Commands:",
    };
    for (CmdInfo<T> const& cmd : TraceCommands<T>) {
        lines.push_back(std::format("    {} {}", cmd.name, cmd.usage));
        lines.push_back(std::format("        {}", cmd.helptext));
    }
    ctx.frontend.showLines(lines, "Worklist Trace Help Menu");
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

template<typename T>
std::vector<CmdInfo<T>> TraceCommands = {
    {"collapse",      "[block num]",                                              "Hide IR for block, making it unavailable for annotation", 1, CmdCollapse<T>},
    {"expand",        "[block num]",                                              "Show IR for block, and make available for annotation", 1, CmdExpand<T>},
    {"child",         "[block num] [child num]",                                  "Show different predecessor blocks", 2, CmdChild<T>},
    {"show",          "[source|ir] [block num]",                                  "Present a preview of IR or a source code approximation of a block", 2, CmdShow<T>},
    {"toggle-view",   "",                                                         "Toggle whether expanded blocks are shown as IR or a source code approximation", 0, CmdToggleView<T>},
    {"fp-target",     "[block num] [instr num]",                                  "Annotate possible function pointer target(s)", 2, CmdFpTarget<T>},
    {"alias-create",  "[yes|no] [block num 1] [value 1] [block num 2] [value 2]", "Annotate if two values should/should not alias. This automatically creates a new alias group.", 5, CmdAliasCreate<T>},
    {"alias-add",     "[group num] [block num] [value]",                          "Add a value to an existing alias group", 3, CmdAliasAdd<T>},
    {"alias-type",    "[group num] [yes|no]",                                     "Set type of an existing alias group", 2, CmdAliasType<T>},
    {"alias-rm",      "[group num] [value num|e<epoch num>]",                     "Remove a value or an epoch (prefixed with e) from an existing alias group (by index in group, see alias-get)", 2, CmdAliasRm<T>},
    {"alias-epoch",   "[group num] [block num] [instr num]",                      "Start a new epoch for an alias group before an instruction.", 3, CmdAliasEpoch<T>},
    {"alias-get",     "",                                                         "Get info on current alias annotations", 0, CmdAliasGet<T>},
    {"alloc-create",  "[block num] [instr num] [size]",                           "Annotate the pointer produced by an instruction as allocated with at least size bytes (0: unknown size)", 3, CmdAllocCreate<T>},
    {"reanalyse",     "",                                                         "Re-run all analyses (e.g. after adding annotations)", 0, CmdReanalyse<T>},
    {"diff",          "[filepath]",                                               "Write out the annotations as a patch file. This includes changes from a previously read patch, if applicable.", 1, CmdDiff<T>},
    {"patch",         "[filepath]",                                               "Read annotations from a patch file", 1, CmdPatch<T>},
    {"dump",          "[filepath]",                                               "Write module to file", 1, CmdDump<T>},
    {"history",       "",                                                         "Print the current commands applied", 0, CmdHistory<T>},
    {"exit",          "",                                                         "Exit the debugger", 0, CmdExit<T>},
    {"quit",          "",                                                         "Exit the debugger", 0, CmdExit<T>},
    {"help",          "",                                                         "Show this help text", 0, CmdHelp<T>},
};

export template<typename T>
std::vector<CmdInfo<T>> const& GetTraceCommands() { return TraceCommands<T>; }

export bool verifyInputArgs(std::string_view const& usage, std::string_view const& input, std::vector<std::string>& args, int const& num_inputs) {
    std::istringstream iss(std::string(input), std::ios_base::in);
    // Throw away first input, which is the command itself
    iss.ignore(std::numeric_limits<std::streamsize>::max(), ' ');
    while (iss && args.size() <= num_inputs) {
        std::string x;
        iss >> x;
        if (!iss) break;
        args.push_back(x);
    }
    if (args.size() != num_inputs)  return false;
    return true;
}

export template<typename T>
CmdResult ExecuteCommand(std::string input_command, CmdContext<T> ctx) {
    CmdResult result;
    std::vector<std::string> args;

    // Get command and verify input sizing
    auto used_cmd = std::find_if(TraceCommands<T>.begin(), TraceCommands<T>.end(), [&](CmdInfo<T> const& cmd){
        return input_command == cmd.name || input_command.starts_with(std::format("{} ", cmd.name));
    });
    if (used_cmd == TraceCommands<T>.end()) {
        result = {CmdResultCode::UNKNOWN_COMMAND, "Unknown command: " + input_command};
        goto return_result;
    }
    if (!verifyInputArgs(used_cmd->name, input_command, args, used_cmd->num_params)) {
        result = {CmdResultCode::INVALID_SYNTAX, std::format("Invalid syntax. Usage: {} {}", used_cmd->name, used_cmd->usage)};
        goto return_result;
    }

    // Run command
    result = used_cmd->handler(args, ctx);

return_result:
    ctx.history.push_back({input_command, result});
    return result;
}

} // namespace InteractiveCmds
