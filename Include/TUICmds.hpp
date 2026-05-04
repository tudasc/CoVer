#pragma once

#include <format>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "TUITraceTypes.hpp"

namespace TUITrace {

#warning todo maybe static struct with help info and stuff, make constructor register itself with static var in TUITrace?

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
CmdResult CmdCollapse(std::vector<std::string>& args, CmdContext<T>& ctx, std::string& last_res) {
    std::optional<int> block = ParseBlock(args[0], ctx, last_res);
    if (!block) return std::nullopt;
    last_res = "Collapsing block " + std::to_string(*block);
    ctx.expand_select[ctx.trace_by_blocks[*block].last_entry] = false;
    return std::nullopt;
}

template<typename T>
CmdResult CmdExpand(std::vector<std::string>& args, CmdContext<T>& ctx, std::string& last_res) {
    std::optional<int> block = ParseBlock(args[0], ctx, last_res);
    if (!block) return std::nullopt;
    last_res = "Expanding block " + std::to_string(*block);
    ctx.expand_select[ctx.trace_by_blocks[*block].last_entry] = true;
    return std::nullopt;
}

template<typename T>
CmdResult CmdChild(std::vector<std::string>& args, CmdContext<T>& ctx, std::string& last_res) {
    std::optional<int> block = ParseBlock(args[0], ctx, last_res);
    if (!block) return std::nullopt;
    int child;
    try { child = std::stoi(args[1]); } catch (...) { last_res = "Child argument is not a number!"; return std::nullopt; }
    TraceBlock<T> selected_block = ctx.trace_by_blocks[*block];
    if (child < 0 || child >= (int)selected_block.last_entry->predecessors.size()) {
        last_res = "Invalid child number! Must be between 0 and " + std::to_string(selected_block.last_entry->predecessors.size() - 1);
        return std::nullopt;
    }
    last_res = "Switching child view for block " + std::to_string(*block) + " to child " + std::to_string(child);
    ctx.sibling_select[selected_block.last_entry] = child;
    return std::nullopt;
}

template<typename T>
CmdResult CmdView(std::vector<std::string>& args, CmdContext<T>& ctx, std::string& last_res) {
    if (args[0] != "source" && args[0] != "ir") {
        last_res = "Unknown source representation: " + args[0];
        return std::nullopt;
    }
    std::optional<int> block = ParseBlock(args[1], ctx, last_res);
    if (!block) return std::nullopt;
    last_res = "";
    ShowBlock(ctx.trace_by_blocks[*block], args[0] == "source", ctx.infoToStr);
    return std::nullopt;
}

template<typename T>
CmdResult CmdFpTarget(std::vector<std::string>& args, CmdContext<T>& ctx, std::string& last_res) {
    std::optional<int> block = ParseBlock(args[0], ctx, last_res);
    if (!block) return std::nullopt;
    int instr;
    try { instr = std::stoi(args[1]); } catch (...) { last_res = "Instruction argument is not a number!"; return std::nullopt; }
    ContractPassUtility::JumpTraceEntry<T>* selected_trace = nullptr;
    int count = 0;
    for (ContractPassUtility::JumpTraceEntry<T>* cur_trace = ctx.trace_by_blocks[*block].first_entry;; cur_trace = cur_trace->predecessors[0]) {
        if (count == instr) { selected_trace = cur_trace; break; }
        if (cur_trace == ctx.trace_by_blocks[*block].last_entry) break;
        count++;
    }
    if (selected_trace == nullptr) { last_res = "No such instruction!"; return std::nullopt; }
    if (!isa<CallBase>(selected_trace->loc) || !dyn_cast<CallBase>(selected_trace->loc)->isIndirectCall()) {
        last_res = "Instruction is not an indirect function call!";
        return std::nullopt;
    }
    std::vector<std::string> funcs;
    Module* M = selected_trace->loc->getModule();
    for (Function const& F : M->functions()) funcs.push_back(F.getName().str());
    std::vector<std::string> existing_annots = ContractPassUtility::getCoVerAnnotations(selected_trace->loc);
    std::set<std::string> previously_selected;
    for (std::string annot : existing_annots) {
        if (annot.starts_with("CoVer_AnnotFP"))
            previously_selected.insert(annot.substr(annot.find("|") + 1));
    }
    std::vector<std::string> sel_funcs = TUIManager::RenderMultiMenu(funcs, "Select Function Target", previously_selected);
    selected_trace->loc->eraseMetadataIf([](uint kind, MDNode* node) {
        if (kind != LLVMContext::MD_annotation) return false;
        MDTuple* Tuple = cast<MDTuple>(node);
        for (MDOperand const& N : Tuple->operands()) {
            if (isa<MDString>(N.get())) {
                std::string annot = cast<MDString>(N.get())->getString().str();
                if (annot.starts_with("CoVer_AnnotFP")) return true;
            }
        }
        return false;
    });
    for (std::string func : sel_funcs)
        selected_trace->loc->addAnnotationMetadata(std::format("CoVer_AnnotFP|{}", func));
    last_res = std::format("Added {} possible target(s) to {}.{}", sel_funcs.size(), *block, instr);
    return std::nullopt;
}

template<typename T>
CmdResult CmdAliasCreate(std::vector<std::string>& args, CmdContext<T>& ctx, std::string& last_res) {
    if (args[0] != "yes" && args[0] != "y" && args[0] != "no" && args[0] != "n") {
        last_res = "Unknown alias type " + args[0];
        return std::nullopt;
    }
    bool isAlias = args[0].starts_with("y");
    std::optional<int> block1 = ParseBlock(args[1], ctx, last_res);
    std::optional<int> block2 = ParseBlock(args[3], ctx, last_res);
    if (!block1 || !block2) return std::nullopt;
    Value* V1 = ContractPassUtility::getValueByName(args[2], ctx.trace_by_blocks[*block1].first_entry->loc->getFunction());
    Value* V2 = ContractPassUtility::getValueByName(args[4], ctx.trace_by_blocks[*block2].first_entry->loc->getFunction());
    if (!V1 || !V2) { last_res = "Could not identify value " + (!V1 ? args[2] : args[4]); return std::nullopt; }
    ContractPassUtility::createAliasGroup(isAlias, V1, V2);
    last_res = std::format("Values {} and {} {} alias", V1->getNameOrAsOperand(), V2->getNameOrAsOperand(), isAlias ? "should" : "should not");
    return std::nullopt;
}

template<typename T>
CmdResult CmdAliasAdd(std::vector<std::string>& args, CmdContext<T>& ctx, std::string& last_res) {
    std::optional<int> group = ParseAliasGroup(args[0], last_res);
    std::optional<int> block = ParseBlock(args[1], ctx, last_res);
    if (!group || !block) return std::nullopt;
    Value* V = ContractPassUtility::getValueByName(args[2], ctx.trace_by_blocks[*block].first_entry->loc->getFunction());
    if (!V) { last_res = "Could not identify value " + args[2]; return std::nullopt; }
    ContractPassUtility::addToAliasGroup(*group, V);
    last_res = std::format("Values {} added to alias group {}", V->getNameOrAsOperand(), *group);
    return std::nullopt;
}

template<typename T>
CmdResult CmdAliasRm(std::vector<std::string>& args, CmdContext<T>& ctx, std::string& last_res) {
    auto group = ParseAliasGroup(args[0], last_res);
    if (!group) return std::nullopt;
    int idx;
    try { idx = std::stoi(args[1]); } catch (...) { last_res = "Group member index argument is not a number!"; return std::nullopt; }
    ContractPassUtility::AliasGroup sel = ContractPassUtility::getAliasAnnots().at(*group);
    if (idx < 0 || idx >= (int)sel.members.size()) {
        last_res = std::format("Group member index must be between 0 and {}", sel.members.size());
        return std::nullopt;
    }
    ContractPassUtility::removeFromAliasGroup(*group, idx);
    last_res = std::format("Removed alias annotation {:>3}.{:0>3}", *group, idx);
    return std::nullopt;
}

template<typename T>
CmdResult CmdAliasGet(std::vector<std::string>&, CmdContext<T>&, std::string&) {
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
    return std::nullopt;
}

template<typename T>
CmdResult CmdReanalyse(std::vector<std::string>&, CmdContext<T>&, std::string&) {
    return true;
}

template<typename T>
CmdResult CmdExit(std::vector<std::string>&, CmdContext<T>&, std::string&) {
    return false;
}

// Defined in TUITrace.hpp after TUICmds.hpp (needed by CmdHelp)
template<typename T> std::vector<CmdInfo<T>> GetTraceCommands();
template<typename T>
CmdResult CmdHelp(std::vector<std::string>&, CmdContext<T>&, std::string&) {
    std::vector<ftxui::Element> lines = {
        ftxui::text("Trace debug menu"),
        ftxui::text(""),
        ftxui::text("Commands:"),
    };
    for (CmdInfo<T> const& cmd : GetTraceCommands<T>()) {
        lines.push_back(ftxui::text(std::format("    {} {}", cmd.name, cmd.usage)));
        lines.push_back(ftxui::text(std::format("        {}", cmd.helptext)));
    }
    TUIManager::ShowLines(lines, "Worklist Trace Help Menu");
    return std::nullopt;
}

} // namespace TUITrace
