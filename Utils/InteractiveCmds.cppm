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
#include <map>
#include <optional>
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
import InteractiveSession;

using namespace llvm;

// Trace debugger commands, shared by the TUI and GUI
namespace InteractiveCmds {

using InteractiveSession::CmdResult;
using InteractiveSession::CmdResultCode;
using InteractiveSession::TraceSession;

// Display hooks for commands that present something to the user, implemented by the TUI and GUI respectively
export struct CmdFrontend {
    std::function<void(std::vector<std::string> const& lines, std::string const& title)> showLines;
    std::function<std::vector<std::string>(std::vector<std::string> const& choices, std::string const& title, std::set<std::string> const& preselected)> selectMultiple;
    std::function<void(InteractiveSession::History const& history)> showHistory;
};

export struct CmdInfo {
    std::string_view name;
    std::string_view usage;
    std::string_view helptext;
    int num_params;
    std::function<CmdResult(std::vector<std::string> const&, TraceSession&, CmdFrontend const&)> handler;
};

export std::vector<CmdInfo> const& GetTraceCommands();

// Parse a whole argument as integer, the error names the argument
std::expected<int, std::string> ParseInt(std::string_view s, std::string_view arg_name) {
    int value;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::unexpected(std::format("{} argument is not a number!", arg_name));
    return value;
}

std::expected<int, std::string> ParseBlock(std::string_view s, TraceSession const& session) {
    std::expected<int, std::string> block = ParseInt(s, "Block");
    if (block && (*block < 0 || *block >= session.Blocks().size()))
        return std::unexpected(std::format("Invalid block number! Must be between 0 and {}", session.Blocks().size() - 1));
    return block;
}

std::expected<Instruction*, std::string> ParseTraceInstr(std::string_view s, int block, TraceSession const& session) {
    std::expected<int, std::string> instr = ParseInt(s, "Instruction");
    if (!instr) return std::unexpected(instr.error());
    Instruction* I = session.BlockInstruction(block, *instr);
    if (!I) return std::unexpected("No such instruction!");
    return I;
}

std::expected<int, std::string> ParseAliasGroup(std::string_view s) {
    std::expected<int, std::string> group = ParseInt(s, "Group");
    if (group && !ContractPassUtility::getAliasAnnots().contains(*group))
        return std::unexpected("Invalid alias group! Check existing groups using alias-get");
    return group;
}

CmdResult CmdCollapse(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::expected<int, std::string> block = ParseBlock(args[0], session);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    session.SetExpanded(*block, false);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Collapsing block {}", *block)};
}

CmdResult CmdExpand(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::expected<int, std::string> block = ParseBlock(args[0], session);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    session.SetExpanded(*block, true);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Expanding block {}", *block)};
}

CmdResult CmdChild(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::expected<int, std::string> block = ParseBlock(args[0], session);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    std::expected<int, std::string> child = ParseInt(args[1], "Child");
    if (!child) return {CmdResultCode::INVALID_INPUT, child.error()};
    int num_children = session.NumChildren(*block);
    if (num_children == 0) return {CmdResultCode::INVALID_INPUT, std::format("Block {} has no children!", *block)};
    if (*child < 0 || *child >= num_children) {
        return {CmdResultCode::INVALID_INPUT, std::format("Invalid child number! Must be between 0 and {}", num_children - 1)};
    }
    session.SelectChild(*block, *child);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Switching child view for block {} to child {}", *block, *child)};
}

CmdResult CmdShow(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    if (args[0] != "source" && args[0] != "ir") {
        return {CmdResultCode::INVALID_INPUT, "Unknown source representation: " + args[0]};
    }
    std::expected<int, std::string> block = ParseBlock(args[1], session);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    bool transToSource = args[0] == "source";
    std::vector<std::string> lines = session.BlockLines(*block, transToSource)
                                   | std::views::transform(&InteractiveUtils::BlockLine::text) | std::ranges::to<std::vector>();
    frontend.showLines(lines, std::format("Block Preview, {}", transToSource ? "Source Approx." : "LLVM IR"));
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

CmdResult CmdToggleView(std::vector<std::string> const&, TraceSession& session, CmdFrontend const& frontend) {
    session.SetExpandAsSource(!session.ExpandAsSource());
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Expanded blocks are now shown as {}", session.ExpandAsSource() ? "source code approximation" : "LLVM IR")};
}

CmdResult CmdFpTarget(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::expected<int, std::string> block = ParseBlock(args[0], session);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    std::expected<Instruction*, std::string> selected = ParseTraceInstr(args[1], *block, session);
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
    std::set<Function*> selected_func = frontend.selectMultiple(funcs, "Select Function Target", previously_selected)
                                      | std::views::transform([&](std::string const& name) { return M->getFunction(name); }) | std::ranges::to<std::set>();
    ContractPassUtility::setFPTarget(CB, selected_func);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("{}.{} now has {} possible target(s)", *block, args[1], selected_func.size())};
}

CmdResult CmdAliasCreate(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    if (args[0] != "yes" && args[0] != "y" && args[0] != "no" && args[0] != "n") {
        return {CmdResultCode::INVALID_INPUT, "Unknown alias type " + args[0]};
    }
    bool isAlias = args[0].starts_with("y");
    std::expected<int, std::string> block1 = ParseBlock(args[1], session);
    if (!block1) return {CmdResultCode::INVALID_INPUT, block1.error()};
    std::expected<int, std::string> block2 = ParseBlock(args[3], session);
    if (!block2) return {CmdResultCode::INVALID_INPUT, block2.error()};
    Value* V1 = ContractPassUtility::getValueByName(args[2], session.BlockFunction(*block1));
    Value* V2 = ContractPassUtility::getValueByName(args[4], session.BlockFunction(*block2));
    if (!V1 || !V2) { return {CmdResultCode::INVALID_INPUT, "Could not identify value " + (!V1 ? args[2] : args[4])}; }
    ContractPassUtility::createAliasGroup(isAlias, V1, V2);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Values {} and {} {} alias", V1->getNameOrAsOperand(), V2->getNameOrAsOperand(), isAlias ? "should" : "should not")};
}

CmdResult CmdAliasAdd(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::expected<int, std::string> group = ParseAliasGroup(args[0]);
    if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
    std::expected<int, std::string> block = ParseBlock(args[1], session);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    Value* V = ContractPassUtility::getValueByName(args[2], session.BlockFunction(*block));
    if (!V) { return {CmdResultCode::INVALID_INPUT, "Could not identify value " + args[2]}; }
    ContractPassUtility::addToAliasGroup(*group, V);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Values {} added to alias group {}", V->getNameOrAsOperand(), *group)};
}

CmdResult CmdAliasType(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::expected<int, std::string> group = ParseAliasGroup(args[0]);
    if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
    bool isAlias = args[1].starts_with("y");
    ContractPassUtility::setAliasGroupType(*group, isAlias);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Alias group {} {}", *group, isAlias ? "should alias" : "should not alias")};
}

CmdResult CmdAliasRm(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
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

CmdResult CmdAliasEpoch(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::expected<int, std::string> group = ParseAliasGroup(args[0]);
    if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
    std::expected<int, std::string> block = ParseBlock(args[1], session);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    std::expected<Instruction*, std::string> selected = ParseTraceInstr(args[2], *block, session);
    if (!selected) return {CmdResultCode::INVALID_INPUT, selected.error()};
    ContractPassUtility::addAliasEpoch(*group, *selected);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Added epoch for alias group {} at {}.{}", *group, *block, args[2])};
}

CmdResult CmdAliasGet(std::vector<std::string> const&, TraceSession& session, CmdFrontend const& frontend) {
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
    frontend.showLines(group_lines, "Current Alias Groups");
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

CmdResult CmdAllocCreate(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::expected<int, std::string> block = ParseBlock(args[0], session);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    std::expected<Instruction*, std::string> selected = ParseTraceInstr(args[1], *block, session);
    if (!selected) return {CmdResultCode::INVALID_INPUT, selected.error()};
    if (args[2].starts_with("-")) return {CmdResultCode::INVALID_INPUT, "Size argument must not be negative!"};
    std::expected<uint64_t, std::string> size = ParseInt(args[2], "Size");
    if (!size) return {CmdResultCode::INVALID_INPUT, size.error()};
    std::string err = ContractPassUtility::setAllocAnnot(*selected, *size);
    if (!err.empty()) return {CmdResultCode::INVALID_INPUT, err};
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("{} at {}.{} is now annotated as allocated{}", (*selected)->getNameOrAsOperand(), *block, args[1], *size ? std::format(" with at least {} bytes", *size) : "")};
}

CmdResult CmdDiff(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::ofstream difffile(args[0]);
    difffile << "|| DIFF HISTORY BEGIN ||\n";
    for (auto const& [command, result] : session.GetHistory()) {
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

CmdResult CmdPatch(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    if (!std::filesystem::exists(args[0])) {
        return {CmdResultCode::INVALID_INPUT, "File does not exist!"};
    }
    std::string err = IRPatch::applyPatch("CoVer_Reanalyse.ll", args[0]);
    if (!err.empty()) return {CmdResultCode::INVALID_INPUT, "Error, " + err};
    return {CmdResultCode::SUCCESS_REANALYSE, "Applied patch from " + args[0]};
}

CmdResult CmdDump(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::error_code EC;
    raw_fd_ostream out(args[0], EC);
    session.GetModule()->print(out, nullptr);
    out << "\n";
    return {CmdResultCode::SUCCESS_CONTINUE, "Wrote module IR to " + args[0]};
}

CmdResult CmdHistory(std::vector<std::string> const&, TraceSession& session, CmdFrontend const& frontend) {
    frontend.showHistory(session.GetHistory());
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

CmdResult CmdReanalyse(std::vector<std::string> const&, TraceSession&, CmdFrontend const& frontend) {
    return {CmdResultCode::SUCCESS_REANALYSE, "Re-running analysis..."};
}

CmdResult CmdExit(std::vector<std::string> const&, TraceSession&, CmdFrontend const& frontend) {
    return {CmdResultCode::SUCCESS_EXIT, "Exiting debugger..."};
}

CmdResult CmdHelp(std::vector<std::string> const&, TraceSession& session, CmdFrontend const& frontend) {
    std::vector<std::string> lines = {
        "Trace debug menu",
        "",
        "Commands:",
    };
    for (CmdInfo const& cmd : GetTraceCommands()) {
        lines.push_back(std::format("    {} {}", cmd.name, cmd.usage));
        lines.push_back(std::format("        {}", cmd.helptext));
    }
    frontend.showLines(lines, "Worklist Trace Help Menu");
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

std::vector<CmdInfo> const TraceCommands = {
    {"collapse",      "[block num]",                                              "Hide IR for block, making it unavailable for annotation", 1, CmdCollapse},
    {"expand",        "[block num]",                                              "Show IR for block, and make available for annotation", 1, CmdExpand},
    {"child",         "[block num] [child num]",                                  "Show different predecessor blocks", 2, CmdChild},
    {"show",          "[source|ir] [block num]",                                  "Present a preview of IR or a source code approximation of a block", 2, CmdShow},
    {"toggle-view",   "",                                                         "Toggle whether expanded blocks are shown as IR or a source code approximation", 0, CmdToggleView},
    {"fp-target",     "[block num] [instr num]",                                  "Annotate possible function pointer target(s)", 2, CmdFpTarget},
    {"alias-create",  "[yes|no] [block num 1] [value 1] [block num 2] [value 2]", "Annotate if two values should/should not alias. This automatically creates a new alias group.", 5, CmdAliasCreate},
    {"alias-add",     "[group num] [block num] [value]",                          "Add a value to an existing alias group", 3, CmdAliasAdd},
    {"alias-type",    "[group num] [yes|no]",                                     "Set type of an existing alias group", 2, CmdAliasType},
    {"alias-rm",      "[group num] [value num|e<epoch num>]",                     "Remove a value or an epoch (prefixed with e) from an existing alias group (by index in group, see alias-get)", 2, CmdAliasRm},
    {"alias-epoch",   "[group num] [block num] [instr num]",                      "Start a new epoch for an alias group before an instruction.", 3, CmdAliasEpoch},
    {"alias-get",     "",                                                         "Get info on current alias annotations", 0, CmdAliasGet},
    {"alloc-create",  "[block num] [instr num] [size]",                           "Annotate the pointer produced by an instruction as allocated with at least size bytes (0: unknown size)", 3, CmdAllocCreate},
    {"reanalyse",     "",                                                         "Re-run all analyses (e.g. after adding annotations)", 0, CmdReanalyse},
    {"diff",          "[filepath]",                                               "Write out the annotations as a patch file. This includes changes from a previously read patch, if applicable.", 1, CmdDiff},
    {"patch",         "[filepath]",                                               "Read annotations from a patch file", 1, CmdPatch},
    {"dump",          "[filepath]",                                               "Write module to file", 1, CmdDump},
    {"history",       "",                                                         "Print the current commands applied", 0, CmdHistory},
    {"exit",          "",                                                         "Exit the debugger", 0, CmdExit},
    {"quit",          "",                                                         "Exit the debugger", 0, CmdExit},
    {"help",          "",                                                         "Show this help text", 0, CmdHelp},
};

std::vector<CmdInfo> const& GetTraceCommands() { return TraceCommands; }

// Arguments following the command name, if there are exactly num_params
std::optional<std::vector<std::string>> SplitArgs(std::string const& input, int num_params) {
    std::istringstream iss(input);
    std::string arg;
    iss >> arg; // Command name
    std::vector<std::string> args;
    while (iss >> arg) args.push_back(arg);
    if (args.size() != num_params) return std::nullopt;
    return args;
}

// Runs a command line, records it in the history, and refreshes the session unless the command ends it
export CmdResult ExecuteCommand(TraceSession& session, std::string const& input_command, CmdFrontend const& frontend) {
    std::vector<CmdInfo>::const_iterator used_cmd = std::ranges::find_if(TraceCommands, [&](CmdInfo const& cmd) {
        return input_command == cmd.name || input_command.starts_with(std::format("{} ", cmd.name));
    });
    CmdResult result;
    if (used_cmd == TraceCommands.end()) {
        result = {CmdResultCode::UNKNOWN_COMMAND, "Unknown command: " + input_command};
    } else if (std::optional<std::vector<std::string>> args = SplitArgs(input_command, used_cmd->num_params)) {
        result = used_cmd->handler(*args, session, frontend);
    } else {
        result = {CmdResultCode::INVALID_SYNTAX, std::format("Invalid syntax. Usage: {} {}", used_cmd->name, used_cmd->usage)};
    }
    session.AddHistory(input_command, result);
    if (result.code != CmdResultCode::SUCCESS_EXIT && result.code != CmdResultCode::SUCCESS_REANALYSE) session.Refresh();
    return result;
}

} // namespace InteractiveCmds
