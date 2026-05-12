#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <iterator>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "TUIManager.hpp"
#include "TUITraceTypes.hpp"
#include "ContractPassUtility.hpp"

namespace TUITrace {

template<typename T>
extern std::vector<CmdInfo<T>> TraceCommands;

template<typename T>
std::optional<int> ParseBlock(std::string const& s, CmdContext<T>& ctx, std::string& last_res) {
    int block;
    try { block = std::stoi(s); } catch (...) { last_res = "Block argument is not a number!"; return std::nullopt; }
    if (block < 0 || block >= ctx.trace_by_blocks.size()) {
        last_res = std::format("Invalid block number! Must be between 0 and {}", ctx.trace_by_blocks.size() - 1);
        return std::nullopt;
    }
    return block;
}

inline std::optional<int> ParseAliasGroup(std::string const& s, std::string& last_res) {
    int group;
    try { group = std::stoi(s); } catch (...) { last_res = "Group argument is not a number!"; return std::nullopt; }
    if (!ContractPassUtility::getAliasAnnots().contains(group)) {
        last_res = "Invalid alias group! Check existing groups using alias-get";
        return std::nullopt;
    }
    return group;
}

template<typename T>
CmdResult CmdCollapse(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::string last_res;
    std::optional<int> block = ParseBlock(args[0], ctx, last_res);
    if (!block) return {CmdResultCode::INVALID_INPUT};
    ctx.expand_select[ctx.trace_by_blocks[*block].last_entry] = false;
    return {CmdResultCode::SUCCESS_CONTINUE, "Collapsing block " + std::to_string(*block)};
}

template<typename T>
CmdResult CmdExpand(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::string last_res;
    std::optional<int> block = ParseBlock(args[0], ctx, last_res);
    if (!block) return {CmdResultCode::INVALID_INPUT, last_res};
    ctx.expand_select[ctx.trace_by_blocks[*block].last_entry] = true;
    return {CmdResultCode::SUCCESS_CONTINUE, "Expanding block " + std::to_string(*block)};
}

template<typename T>
CmdResult CmdChild(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::string last_res;
    std::optional<int> block = ParseBlock(args[0], ctx, last_res);
    if (!block) return {CmdResultCode::INVALID_INPUT, last_res};
    int child;
    try { child = std::stoi(args[1]); } catch (...) { return {CmdResultCode::INVALID_INPUT, "Child argument is not a number!"}; }
    TraceBlock<T> selected_block = ctx.trace_by_blocks[*block];
    if (child < 0 || child >= (int)selected_block.last_entry->predecessors.size()) {
        return {CmdResultCode::INVALID_INPUT, "Invalid child number! Must be between 0 and " + std::to_string(selected_block.last_entry->predecessors.size() - 1)};
    }
    ctx.sibling_select[selected_block.last_entry] = child;
    return {CmdResultCode::SUCCESS_CONTINUE, "Switching child view for block " + std::to_string(*block) + " to child " + std::to_string(child)};
}

template<typename T>
CmdResult CmdView(std::vector<std::string>& args, CmdContext<T>& ctx) {
    if (args[0] != "source" && args[0] != "ir") {
        return {CmdResultCode::INVALID_INPUT, "Unknown source representation: " + args[0]};
    }
    std::string last_res;
    std::optional<int> block = ParseBlock(args[1], ctx, last_res);
    if (!block) return {CmdResultCode::INVALID_INPUT, last_res};
    ShowBlock(ctx.trace_by_blocks[*block], args[0] == "source", ctx.infoToStr);
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

template<typename T>
CmdResult CmdFpTarget(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::string last_res;
    std::optional<int> block = ParseBlock(args[0], ctx, last_res);
    if (!block) return {CmdResultCode::INVALID_INPUT, last_res};
    int instr;
    try { instr = std::stoi(args[1]); } catch (...) { return {CmdResultCode::INVALID_INPUT, "Instruction argument is not a number!"}; }
    ContractPassUtility::JumpTraceEntry<T>* selected_trace = nullptr;
    int count = 0;
    for (ContractPassUtility::JumpTraceEntry<T>* cur_trace = ctx.trace_by_blocks[*block].first_entry;; cur_trace = cur_trace->predecessors[0]) {
        if (count == instr) { selected_trace = cur_trace; break; }
        if (cur_trace == ctx.trace_by_blocks[*block].last_entry) break;
        count++;
    }
    if (selected_trace == nullptr) { return {CmdResultCode::INVALID_INPUT, "No such instruction!"}; }
    if (!isa<CallBase>(selected_trace->loc) || !dyn_cast<CallBase>(selected_trace->loc)->isIndirectCall()) {
        return {CmdResultCode::INVALID_INPUT, "Instruction is not an indirect function call!"};
    }
    std::vector<std::string> funcs;
    Module* M = selected_trace->loc->getModule();
    for (Function const& F : M->functions()) {
        if (!F.getName().starts_with("CoVer_") && !F.getName().starts_with("llvm.")) funcs.push_back(F.getName().str());
    }
    std::set<Function*> selected_func = ContractPassUtility::getFPAnnots(dyn_cast<CallBase>(selected_trace->loc));
    std::set<std::string> previously_selected;
    std::transform(selected_func.begin(), selected_func.end(), std::inserter(previously_selected, previously_selected.end()), [](Function* F){ return F->getName().str(); });
    std::vector<std::string> sel_func_str = TUIManager::RenderMultiMenu(funcs, "Select Function Target", previously_selected);
    selected_func.clear();
    std::transform(sel_func_str.begin(), sel_func_str.end(), std::inserter(selected_func, selected_func.end()), [&](std::string Fstr){ return M->getFunction(Fstr); });
    ContractPassUtility::setFPTarget(dyn_cast<CallBase>(selected_trace->loc), selected_func);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("{}.{} now has {} possible target(s)", *block, instr, selected_func.size())};
}

template<typename T>
CmdResult CmdAliasCreate(std::vector<std::string>& args, CmdContext<T>& ctx) {
    if (args[0] != "yes" && args[0] != "y" && args[0] != "no" && args[0] != "n") {
        return {CmdResultCode::INVALID_INPUT, "Unknown alias type " + args[0]};
    }
    bool isAlias = args[0].starts_with("y");
    std::string last_res;
    std::optional<int> block1 = ParseBlock(args[1], ctx, last_res);
    std::optional<int> block2 = ParseBlock(args[3], ctx, last_res);
    if (!block1 || !block2) return {CmdResultCode::INVALID_INPUT, last_res};
    Value* V1 = ContractPassUtility::getValueByName(args[2], ctx.trace_by_blocks[*block1].first_entry->loc->getFunction());
    Value* V2 = ContractPassUtility::getValueByName(args[4], ctx.trace_by_blocks[*block2].first_entry->loc->getFunction());
    if (!V1 || !V2) { return {CmdResultCode::INVALID_INPUT, "Could not identify value " + (!V1 ? args[2] : args[4])}; }
    ContractPassUtility::createAliasGroup(isAlias, V1, V2);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Values {} and {} {} alias", V1->getNameOrAsOperand(), V2->getNameOrAsOperand(), isAlias ? "should" : "should not")};
}

template<typename T>
CmdResult CmdAliasAdd(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::string last_res;
    std::optional<int> group = ParseAliasGroup(args[0], last_res);
    std::optional<int> block = ParseBlock(args[1], ctx, last_res);
    if (!group || !block) return {CmdResultCode::INVALID_INPUT, last_res};
    Value* V = ContractPassUtility::getValueByName(args[2], ctx.trace_by_blocks[*block].first_entry->loc->getFunction());
    if (!V) { return {CmdResultCode::INVALID_INPUT, "Could not identify value " + args[2]}; }
    ContractPassUtility::addToAliasGroup(*group, V);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Values {} added to alias group {}", V->getNameOrAsOperand(), *group)};
}

template<typename T>
CmdResult CmdAliasRm(std::vector<std::string>& args, CmdContext<T>& ctx) {
    std::string last_res;
    auto group = ParseAliasGroup(args[0], last_res);
    if (!group) return {CmdResultCode::INVALID_INPUT, last_res};
    int idx;
    try { idx = std::stoi(args[1]); } catch (...) { return {CmdResultCode::INVALID_INPUT, "Group member index argument is not a number!"}; }
    ContractPassUtility::AliasGroup sel = ContractPassUtility::getAliasAnnots().at(*group);
    if (idx < 0 || idx >= (int)sel.members.size()) {
        return {CmdResultCode::INVALID_INPUT, std::format("Group member index must be between 0 and {}", sel.members.size())};
    }
    ContractPassUtility::removeFromAliasGroup(*group, idx);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Removed alias annotation {:>3}.{:0>3}", *group, idx)};
}

template<typename T>
CmdResult CmdAliasGet(std::vector<std::string>&, CmdContext<T>&) {
    std::map<int, ContractPassUtility::AliasGroup> const AGs = ContractPassUtility::getAliasAnnots();
    std::vector<ftxui::Element> group_lines;
    for (std::pair<int, ContractPassUtility::AliasGroup> const& G : AGs) {
        group_lines.push_back(ftxui::text(std::format("Group {}, {}", G.first, G.second.areAliasing ? "should alias" : "should not alias")));
        int num = 0;
        for (Value const* V : G.second.members) {
            std::string out;
            raw_string_ostream strstream(out);
            V->print(strstream);
            group_lines.push_back(ftxui::text(std::format("{}.{:0>3} | {}", G.first, num++, out)));
        }
    }
    TUIManager::ShowLines(group_lines, "Current Alias Groups");
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

template<typename T>
CmdResult CmdDiff(std::vector<std::string>& args, CmdContext<T>& ctx) {
    Module* M = ctx.trace_by_blocks.begin()->first_entry->loc->getModule();
    int rc = std::system(std::format("diff -c -I '^; ModuleID' CoVer_InteractStart.ll CoVer_Reanalyse.ll > {}", args[0]).c_str());
    if (!WIFEXITED(rc) || (WEXITSTATUS(rc) && WEXITSTATUS(rc) != 1)) return {CmdResultCode::INVALID_INPUT, std::format("Error, diff returned exit code {}", WEXITSTATUS(rc))};
    return {CmdResultCode::SUCCESS_CONTINUE, "Wrote patch file to " + args[0]};
}

template<typename T>
CmdResult CmdPatch(std::vector<std::string>& args, CmdContext<T>& ctx) {
    if (!std::filesystem::exists(args[0])) {
        return {CmdResultCode::INVALID_INPUT, "File does not exist!"};
    }
    int rc = std::system(std::format("patch CoVer_Reanalyse.ll < {}", args[0]).c_str());
    if ((!WIFEXITED(rc) || WEXITSTATUS(rc))) return {CmdResultCode::INVALID_INPUT, std::format("Error, patch returned exit code {}", WEXITSTATUS(rc))};
    return {CmdResultCode::SUCCESS_REANALYSE, "Applied patch from " + args[0]};
}

template<typename T>
CmdResult CmdHistory(std::vector<std::string>&, CmdContext<T>& ctx) {
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
    for (int i = 0; i < ctx.history.size(); i++) {
        ftxui::Element result_str;
        switch (ctx.history[i].second.code) {
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
            ftxui::text(ctx.history[i].first)
        }));
    }
    TUIManager::ShowLines(lines, "Command History");
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

template<typename T>
CmdResult CmdReanalyse(std::vector<std::string>&, CmdContext<T>&) {
    return {CmdResultCode::SUCCESS_REANALYSE, "Re-running analysis..."};
}

template<typename T>
CmdResult CmdExit(std::vector<std::string>&, CmdContext<T>&) {
    return {CmdResultCode::SUCCESS_EXIT, "Exiting TUI..."};
}

template<typename T>
CmdResult CmdHelp(std::vector<std::string>&, CmdContext<T>&) {
    std::vector<ftxui::Element> lines = {
        ftxui::text("Trace debug menu"),
        ftxui::text(""),
        ftxui::text("Commands:"),
    };
    for (CmdInfo<T> const& cmd : TraceCommands<T>) {
        lines.push_back(ftxui::text(std::format("    {} {}", cmd.name, cmd.usage)));
        lines.push_back(ftxui::text(std::format("        {}", cmd.helptext)));
    }
    TUIManager::ShowLines(lines, "Worklist Trace Help Menu");
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

template<typename T>
std::vector<CmdInfo<T>> TraceCommands = {
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
        {"diff",          "[filepath]",                                               "Write out the annotations as a patch file. This includes changes from a previously read patch, if applicable.", 1, CmdDiff<T>},
        {"patch",         "[filepath]",                                               "Read annotations from a patch file", 1, CmdPatch<T>},
        {"history",       "",                                                         "Print the current commands applied", 0, CmdHistory<T>},
        {"exit",          "",                                                         "Exit the debugger", 0, CmdExit<T>},
        {"quit",          "",                                                         "Exit the debugger", 0, CmdExit<T>},
        {"help",          "",                                                         "Show this help text", 0, CmdHelp<T>},
};

template<typename T>
CmdResult ExecuteTUICommand(std::string input_command, CmdContext<T> ctx) {
    CmdResult result;
    std::vector<std::string> args;

    // Get command and verify input sizing
    auto used_cmd = std::find_if(TraceCommands<T>.begin(), TraceCommands<T>.end(), [&](CmdInfo<T> const& cmd){
        return input_command == cmd.name || input_command.starts_with(std::format("{} ", cmd.name));
    });
    if (used_cmd == TraceCommands<T>.end()) {
        result = {CmdResultCode::UNKNOWN_COMMAND, "Unknown command: " + input_command};
        goto tui_return_result;
    }
    if (!verifyInputArgs(used_cmd->name, input_command, args, used_cmd->num_params)) {
        result = {CmdResultCode::INVALID_SYNTAX, std::format("Invalid syntax. Usage: {} {}", used_cmd->name, used_cmd->usage)};
        goto tui_return_result;
    }

    // Run command
    result = used_cmd->handler(args, ctx);

tui_return_result:
    ctx.history.push_back({input_command, result});
    return result;
}

} // namespace TUITrace
