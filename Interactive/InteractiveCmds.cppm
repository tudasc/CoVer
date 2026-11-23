export module InteractiveCmds;
import std;
import LLVMModule;
import ContractPassUtility;
import ContractMatching;
import IRPatch;
import InteractiveUtils;
import InteractiveSession;

using namespace llvm;

// Trace debugger commands, shared by the TUI and GUI
namespace InteractiveCmds {

using InteractiveSession::CmdResult;
using InteractiveSession::CmdResultCode;
using InteractiveSession::TraceSession;

// Pages of the settings command
export constexpr std::array SETTINGS_PAGES = std::to_array<std::string_view>({"llm"});

// Display hooks for commands that present something to the user, implemented by the TUI and GUI respectively
export struct CmdFrontend {
    std::function<void(std::vector<std::string> const& lines, std::string const& title)> showLines;
    std::function<std::vector<std::string>(std::vector<std::string> const& choices, std::string const& title, std::set<std::string> const& preselected)> selectMultiple;
    std::function<void(InteractiveSession::History const& history)> showHistory;
    std::function<void()> openChat;
    std::function<CmdResult(std::string const& page)> openSettings;
    std::function<CmdResult()> openMCP;
};

export enum struct CmdAccess {
    ANY,
    USER, // Only the user: ends session, touches files chosen by user, needs interactive selection, or changes the view
    FRONTEND, // Only the user: opens a feature of the frontend instead of working on the trace
};

// What a command works on: the analysed module, and the open trace if there is one
export struct CmdTarget {
    Module& module;
    TraceSession* trace = nullptr;
};

using TraceHandler = std::function<CmdResult(std::vector<std::string> const&, TraceSession&, CmdFrontend const&)>;
// Commands that only work on the module, which also run without an open trace. Block numbers need one, function names do not.
using ModuleHandler = std::function<CmdResult(std::vector<std::string> const&, CmdTarget const&, CmdFrontend const&)>;

export struct CmdInfo {
    std::string_view name;
    std::string_view usage;
    std::string_view helptext;
    int num_params;
    CmdAccess access;
    std::variant<TraceHandler, ModuleHandler> handler;
    bool variadic = false;

    bool NeedsTrace() const { return std::holds_alternative<TraceHandler>(handler); }
};

export std::vector<CmdInfo> const& GetTraceCommands();

// Parse a whole argument as integer, the error names the argument
std::expected<int, std::string> ParseIntArg(std::string_view s, std::string_view arg_name) {
    std::optional<int> value = ContractPassUtility::parseInt(s);
    if (!value) return std::unexpected(std::format("{} argument is not a number!", arg_name));
    return *value;
}

std::expected<int, std::string> ParseBlock(std::string_view s, TraceSession const& session) {
    std::expected<int, std::string> block = ParseIntArg(s, "Block");
    if (block && (*block < 0 || *block >= session.Blocks().size()))
        return std::unexpected(std::format("Invalid block number! Must be between 0 and {}", session.Blocks().size() - 1));
    return block;
}

std::expected<Instruction*, std::string> ParseTraceInstr(std::string_view s, int block, TraceSession const& session) {
    std::expected<int, std::string> instr = ParseIntArg(s, "Instruction");
    if (!instr) return std::unexpected(instr.error());
    Instruction* I = session.BlockInstruction(block, *instr);
    if (!I) return std::unexpected("No such instruction!");
    return I;
}

// A block number of the open trace, for commands that also run without one
std::expected<int, std::string> ParseBlock(std::string_view s, CmdTarget const& target) {
    if (!target.trace) return std::unexpected("No trace is open, give a function name instead of a block number");
    return ParseBlock(s, *target.trace);
}

std::expected<Function*, std::string> ParseValueFunction(std::string_view s, CmdTarget const& target) {
    if (ContractPassUtility::parseInt(s)) {
        std::expected<int, std::string> block = ParseBlock(s, target);
        if (!block) return std::unexpected(block.error());
        return target.trace->BlockFunction(*block);
    }
    if (s.starts_with("@")) s.remove_prefix(1);
    Function* F = target.module.getFunction(s);
    if (!F) return std::unexpected(std::format("No function named {} in the module!", s));
    if (F->isDeclaration()) return std::unexpected(std::format("Function {} is only declared, it has no values to annotate!", s));
    return F;
}

// A value to annotate as alias group member, which CoVer_AnnotAlias takes as pointer
std::expected<Value*, std::string> ParseValue(std::string const& name, Function* F) {
    Value* V = ContractPassUtility::getValueByName(name, F);
    if (!V) return std::unexpected(std::format("Could not identify value {} in function {}", name, F->getName().str()));
    if (!V->getType()->isPointerTy()) {
        std::string type;
        raw_string_ostream(type) << *V->getType();
        return std::unexpected(std::format("Value {} in function {} is not a pointer but {}, only pointers can be annotated", name, F->getName().str(), type));
    }
    return V;
}

std::string JoinNames(std::vector<std::string> const& names) {
    return names | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>();
}

std::expected<int, std::string> ParseAliasGroup(std::string_view s) {
    std::expected<int, std::string> group = ParseIntArg(s, "Group");
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
    std::expected<int, std::string> child = ParseIntArg(args[1], "Child");
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
                                   | std::views::transform(&InteractiveSession::BlockLine::text) | std::ranges::to<std::vector>();
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
    std::set<Function*> selected_func;
    for (std::string const& name : frontend.selectMultiple(funcs, "Select Function Target", previously_selected)) {
        Function* F = M->getFunction(name);
        if (!F || !std::ranges::contains(funcs, name)) return {CmdResultCode::INVALID_INPUT, "Unknown function target " + name};
        selected_func.insert(F);
    }
    ContractPassUtility::setFPTarget(CB, selected_func);
    ContractMatching::Initialize(); // The call chains through CB changed
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("{}.{} now has {} possible target(s)", *block, args[1], selected_func.size())};
}

CmdResult CmdAliasCreate(std::vector<std::string> const& args, CmdTarget const& target, CmdFrontend const& frontend) {
    if (args[0] != "yes" && args[0] != "y" && args[0] != "no" && args[0] != "n") {
        return {CmdResultCode::INVALID_INPUT, "Unknown alias type " + args[0]};
    }
    bool isAlias = args[0].starts_with("y");
    std::expected<Function*, std::string> F1 = ParseValueFunction(args[1], target);
    if (!F1) return {CmdResultCode::INVALID_INPUT, F1.error()};
    std::expected<Function*, std::string> F2 = ParseValueFunction(args[3], target);
    if (!F2) return {CmdResultCode::INVALID_INPUT, F2.error()};
    std::expected<Value*, std::string> V1 = ParseValue(args[2], *F1);
    if (!V1) return {CmdResultCode::INVALID_INPUT, V1.error()};
    std::expected<Value*, std::string> V2 = ParseValue(args[4], *F2);
    if (!V2) return {CmdResultCode::INVALID_INPUT, V2.error()};
    // Named before the edit, which discards the value numbering
    std::string values = std::format("values {} and {} {} alias", ContractPassUtility::getValueName(*V1), ContractPassUtility::getValueName(*V2), isAlias ? "should" : "should not");
    int group = ContractPassUtility::createAliasGroup(isAlias, *V1, *V2);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Created alias group {}: {}", group, values)};
}

CmdResult CmdAliasAdd(std::vector<std::string> const& args, CmdTarget const& target, CmdFrontend const& frontend) {
    std::expected<int, std::string> group = ParseAliasGroup(args[0]);
    if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
    std::expected<Function*, std::string> F = ParseValueFunction(args[1], target);
    if (!F) return {CmdResultCode::INVALID_INPUT, F.error()};
    // All values are looked up before adding any, so that a wrong one does not leave the group partially extended
    std::vector<Value*> values;
    for (std::string const& name : args | std::views::drop(2)) {
        std::expected<Value*, std::string> V = ParseValue(name, *F);
        if (!V) return {CmdResultCode::INVALID_INPUT, V.error()};
        values.push_back(*V);
    }
    // Named before the edits, which discard the value numbering
    std::vector<std::string> names = values | std::views::transform(ContractPassUtility::getValueName) | std::ranges::to<std::vector>();
    std::vector<std::string> added;
    std::vector<std::string> present;
    for (auto const& [V, name] : std::views::zip(values, names)) {
        (ContractPassUtility::getAliasAnnots().at(*group).members.contains(V) ? present : added).push_back(name);
        ContractPassUtility::addToAliasGroup(*group, V);
    }
    std::string res = added.empty() ? std::format("No values added to alias group {}", *group) : std::format("Values {} added to alias group {}", JoinNames(added), *group);
    if (!present.empty()) res += std::format(", already in the group: {}", JoinNames(present));
    return {CmdResultCode::SUCCESS_CONTINUE, res};
}

CmdResult CmdAliasType(std::vector<std::string> const& args, CmdTarget const& target, CmdFrontend const& frontend) {
    std::expected<int, std::string> group = ParseAliasGroup(args[0]);
    if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
    bool isAlias = args[1].starts_with("y");
    ContractPassUtility::setAliasGroupType(*group, isAlias);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Alias group {} {}", *group, isAlias ? "should alias" : "should not alias")};
}

CmdResult CmdAliasRm(std::vector<std::string> const& args, CmdTarget const& target, CmdFrontend const& frontend) {
    std::expected<int, std::string> group = ParseAliasGroup(args[0]);
    if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
    std::set<Value*, std::less<>> const& members = ContractPassUtility::getAliasAnnots().at(*group).members;
    int num_epochs = ContractPassUtility::getAliasEpochs(*group).size();
    // All indices are checked before removing any, so that a wrong one leaves the group as it is
    std::set<int> member_indices;
    std::set<int> epoch_indices;
    for (std::string const& arg : args | std::views::drop(1)) {
        // An index or a range like 2-5, prefixed with e for epochs
        bool epoch = arg.starts_with("e");
        std::string_view prefix = epoch ? "e" : "";
        std::string_view what = epoch ? "Epoch index" : "Group member index";
        int num_entries = epoch ? num_epochs : members.size();
        std::string_view range = std::string_view(arg).substr(prefix.size());
        std::size_t dash = range.find('-', 1); // Not a leading minus
        std::expected<int, std::string> first = ParseIntArg(range.substr(0, dash), what);
        if (!first) return {CmdResultCode::INVALID_INPUT, first.error()};
        std::expected<int, std::string> last = dash == std::string_view::npos ? first : ParseIntArg(range.substr(dash + 1), what);
        if (!last) return {CmdResultCode::INVALID_INPUT, last.error()};
        if (*first > *last) return {CmdResultCode::INVALID_INPUT, std::format("Invalid range {}, the first index is larger than the last", arg)};
        if (num_entries == 0) return {CmdResultCode::INVALID_INPUT, std::format("The alias group has no {}", epoch ? "epochs" : "members")};
        if (*first < 0 || *last >= num_entries) return {CmdResultCode::INVALID_INPUT, std::format("{} must be between {}0 and {}{}", what, prefix, prefix, num_entries - 1)};
        std::set<int>& indices = epoch ? epoch_indices : member_indices;
        for (int idx = *first; idx <= *last; idx++) indices.insert(idx);
    }
    // Named before the edits, which shift the indices of the following entries and discard the value numbering
    std::vector<std::string> removed;
    for (int idx : member_indices) removed.push_back(std::format("{}.{:0>3} ({})", *group, idx, ContractPassUtility::getValueName(*std::next(members.begin(), idx))));
    for (int idx : epoch_indices) removed.push_back(std::format("{}.e{:0>2} (epoch)", *group, idx));
    bool deletes_group = member_indices.size() == members.size();
    // Backwards, so that the indices still to be removed stay valid. Epochs first, removing the last member deletes the group with its epochs.
    for (int idx : epoch_indices | std::views::reverse) ContractPassUtility::removeAliasEpoch(*group, idx);
    for (int idx : member_indices | std::views::reverse) ContractPassUtility::removeFromAliasGroup(*group, idx);
    std::string res = std::format("Removed from alias group {}: {}", *group, JoinNames(removed));
    if (deletes_group) res += ". The group has no members left and was deleted with its epochs";
    return {CmdResultCode::SUCCESS_CONTINUE, res};
}

CmdResult CmdAliasEpoch(std::vector<std::string> const& args, CmdTarget const& target, CmdFrontend const& frontend) {
    std::expected<int, std::string> group = ParseAliasGroup(args[0]);
    if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
    // An instruction number refers to the trace block, anything else is a value of the function
    if (ContractPassUtility::parseInt(args[2])) {
        std::expected<int, std::string> block = ParseBlock(args[1], target);
        if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
        std::expected<Instruction*, std::string> selected = ParseTraceInstr(args[2], *block, *target.trace);
        if (!selected) return {CmdResultCode::INVALID_INPUT, selected.error()};
        ContractPassUtility::addAliasEpoch(*group, *selected);
        return {CmdResultCode::SUCCESS_CONTINUE, std::format("Added epoch for alias group {} at {}.{}", *group, *block, args[2])};
    }
    std::expected<Function*, std::string> F = ParseValueFunction(args[1], target);
    if (!F) return {CmdResultCode::INVALID_INPUT, F.error()};
    Value* V = ContractPassUtility::getValueByName(args[2], *F);
    if (!V) return {CmdResultCode::INVALID_INPUT, std::format("Could not identify value {} in function {}", args[2], (*F)->getName().str())};
    // Named before the edit, which discards the value numbering
    std::string where;
    Instruction* before = nullptr;
    if (Instruction* I = dyn_cast<Instruction>(V)) {
        before = I;
        where = std::format("before {} in {}", ContractPassUtility::getValueName(I), (*F)->getName().str());
    } else if (isa<Argument>(V)) {
        before = &*(*F)->getEntryBlock().getFirstInsertionPt();
        where = std::format("at the start of {}", (*F)->getName().str());
    } else {
        return {CmdResultCode::INVALID_INPUT, std::format("Value {} is neither an instruction nor an argument of function {}", args[2], (*F)->getName().str())};
    }
    ContractPassUtility::addAliasEpoch(*group, before);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("Added epoch for alias group {} {}", *group, where)};
}

export struct AliasGroupEntry {
    std::string arg; // Index as accepted by alias-rm
    std::string label; // Padded index
    std::string text;
    std::string function; // Of a member that is an instruction or argument, empty otherwise
};

// A member of an alias group as compact IR with its source location. Globals only by name, their initialiser may be long.
std::string AliasMemberText(Value const* V) {
    if (GlobalValue const* GV = dyn_cast<GlobalValue>(V)) return std::format("@{}    ; global", GV->getName().str());
    std::string ir = InteractiveUtils::CompactIR(ContractPassUtility::printValue(V));
    ir.erase(0, ir.find_first_not_of(' '));
    if (Argument const* A = dyn_cast<Argument>(V)) return std::format("{}    ; argument {}", ir, A->getArgNo());
    Instruction const* I = dyn_cast<Instruction>(V);
    if (I && I->getDebugLoc()) return std::format("{}    ; {}", ir, ContractPassUtility::getInstrLocStr(I, false));
    return ir;
}

// Members followed by epochs of an alias group
export std::vector<AliasGroupEntry> getAliasGroupEntries(int group) {
    std::vector<AliasGroupEntry> entries;
    int num = 0;
    for (Value const* V : ContractPassUtility::getAliasAnnots().at(group).members) {
        Function const* F = ContractPassUtility::getParentFunction(V);
        entries.push_back({std::to_string(num), std::format("{:0>3}", num), AliasMemberText(V), F ? F->getName().str() : ""});
        num++;
    }
    num = 0;
    for (CallBase const* CB : ContractPassUtility::getAliasEpochs(group)) {
        entries.push_back({std::format("e{}", num), std::format("e{:0>2}", num), std::format("epoch in {} at {}", CB->getFunction()->getName().str(), ContractPassUtility::getInstrLocStr(CB))});
        num++;
    }
    return entries;
}

CmdResult CmdAliasGet(std::vector<std::string> const& args, CmdTarget const& target, CmdFrontend const& frontend) {
    std::set<int> groups;
    for (std::string const& arg : args) {
        std::expected<int, std::string> group = ParseAliasGroup(arg);
        if (!group) return {CmdResultCode::INVALID_INPUT, group.error()};
        groups.insert(*group);
    }
    if (args.empty()) groups = ContractPassUtility::getAliasAnnots() | std::views::keys | std::ranges::to<std::set>();
    std::vector<std::string> group_lines;
    for (int group : groups) {
        group_lines.push_back(std::format("Group {}, {}", group, ContractPassUtility::getAliasAnnots().at(group).areAliasing ? "should alias" : "should not alias"));
        std::string function; // The value names of the following members refer to it
        for (AliasGroupEntry const& entry : getAliasGroupEntries(group)) {
            if (!entry.function.empty() && entry.function != function) group_lines.push_back(std::format("  in {}:", function = entry.function));
            group_lines.push_back(std::format("{}.{} | {}", group, entry.label, entry.text));
        }
    }
    if (group_lines.empty()) group_lines.push_back("No alias groups");
    frontend.showLines(group_lines, groups.size() == ContractPassUtility::getAliasAnnots().size() ? "Current Alias Groups" : "Selected Alias Groups");
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

CmdResult CmdAliasCheck(std::vector<std::string> const& args, CmdTarget const& target, CmdFrontend const& frontend) {
    std::expected<Function*, std::string> F1 = ParseValueFunction(args[0], target);
    if (!F1) return {CmdResultCode::INVALID_INPUT, F1.error()};
    std::expected<Function*, std::string> F2 = ParseValueFunction(args[2], target);
    if (!F2) return {CmdResultCode::INVALID_INPUT, F2.error()};
    std::expected<Value*, std::string> V1 = ParseValue(args[1], *F1);
    if (!V1) return {CmdResultCode::INVALID_INPUT, V1.error()};
    std::expected<Value*, std::string> V2 = ParseValue(args[3], *F2);
    if (!V2) return {CmdResultCode::INVALID_INPUT, V2.error()};
    // Compared directly, like a contract parameter with the parameter of a call or the address of a store
    ContractMatching::ParamMatch result = ContractMatching::checkParamMatch(*V1, *V2);
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("{} and {} {}: {}", ContractPassUtility::getValueName(*V1), ContractPassUtility::getValueName(*V2),
                                                         result.match ? "match" : "do not match", result.Explain())};
}

CmdResult CmdAllocCreate(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::expected<int, std::string> block = ParseBlock(args[0], session);
    if (!block) return {CmdResultCode::INVALID_INPUT, block.error()};
    std::expected<Instruction*, std::string> selected = ParseTraceInstr(args[1], *block, session);
    if (!selected) return {CmdResultCode::INVALID_INPUT, selected.error()};
    if (args[2].starts_with("-")) return {CmdResultCode::INVALID_INPUT, "Size argument must not be negative!"};
    std::expected<std::uint64_t, std::string> size = ParseIntArg(args[2], "Size");
    if (!size) return {CmdResultCode::INVALID_INPUT, size.error()};
    std::string name = ContractPassUtility::getValueName(*selected); // Before the edit, which discards the value numbering
    std::string err = ContractPassUtility::setAllocAnnot(*selected, *size);
    if (!err.empty()) return {CmdResultCode::INVALID_INPUT, err};
    return {CmdResultCode::SUCCESS_CONTINUE, std::format("{} at {}.{} is now annotated as allocated{}", name, *block, args[1], *size ? std::format(" with at least {} bytes", *size) : "")};
}

// Default file of the patch files written by the frontends
export constexpr char const* PATCH_FILE = "annotations.patch";

// Writes the annotations of this and previous runs as a patch file, see the diff command. Also usable without an open trace.
export CmdResult WritePatch(std::string const& path) {
    // Written once a trace is opened or a reanalysis starts, before that the module has no annotations
    char const* current = std::filesystem::exists(InteractiveUtils::REANALYSE_FILE) ? InteractiveUtils::REANALYSE_FILE : InteractiveUtils::INTERACT_START_FILE;
    std::ofstream difffile(path);
    if (!difffile) return {CmdResultCode::INVALID_INPUT, "Error, cannot write to " + path};
    InteractiveSession::WriteHistory(difffile, false);
    std::string err = IRPatch::writePatch(InteractiveUtils::INTERACT_START_FILE, current, difffile);
    if (!err.empty()) return {CmdResultCode::INVALID_INPUT, "Error, " + err};
    return {CmdResultCode::SUCCESS_CONTINUE, "Wrote patch file to " + path};
}

CmdResult CmdDiff(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    return WritePatch(args[0]);
}

CmdResult CmdPatch(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    if (!std::filesystem::exists(args[0])) {
        return {CmdResultCode::INVALID_INPUT, "File does not exist!"};
    }
    std::string err = IRPatch::applyPatch(InteractiveUtils::REANALYSE_FILE, args[0]);
    if (!err.empty()) return {CmdResultCode::INVALID_INPUT, "Error, " + err};
    return {CmdResultCode::SUCCESS_REANALYSE, "Applied patch from " + args[0]};
}

CmdResult CmdDump(std::vector<std::string> const& args, TraceSession& session, CmdFrontend const& frontend) {
    std::error_code EC;
    raw_fd_ostream out(args[0], EC);
    ContractPassUtility::printWithoutRemovedAnnots(*session.GetModule(), out);
    out << "\n";
    return {CmdResultCode::SUCCESS_CONTINUE, "Wrote module IR to " + args[0]};
}

CmdResult CmdHistory(std::vector<std::string> const&, CmdTarget const&, CmdFrontend const& frontend) {
    frontend.showHistory(InteractiveSession::CommandHistory());
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

CmdResult CmdChat(std::vector<std::string> const&, TraceSession&, CmdFrontend const& frontend) {
    frontend.openChat();
    return {CmdResultCode::SUCCESS_CONTINUE, "Closed LLM chat"};
}

CmdResult CmdSettings(std::vector<std::string> const& args, TraceSession&, CmdFrontend const& frontend) {
    if (!std::ranges::contains(SETTINGS_PAGES, args[0])) {
        return {CmdResultCode::INVALID_INPUT, std::format("Unknown settings page {}", args[0])};
    }
    return frontend.openSettings(args[0]);
}

CmdResult CmdMCP(std::vector<std::string> const&, TraceSession&, CmdFrontend const& frontend) {
    return frontend.openMCP();
}

CmdResult CmdReanalyse(std::vector<std::string> const&, TraceSession&, CmdFrontend const& frontend) {
    return {CmdResultCode::SUCCESS_REANALYSE, "Re-running analysis..."};
}

CmdResult CmdExit(std::vector<std::string> const&, TraceSession&, CmdFrontend const& frontend) {
    return {CmdResultCode::SUCCESS_EXIT, "Exiting debugger..."};
}

CmdResult CmdHelp(std::vector<std::string> const&, CmdTarget const&, CmdFrontend const& frontend) {
    std::vector<std::string> lines = {
        "Trace debug menu",
        "",
        "Commands:",
    };
    // Grouped by access, as the LLM chat and MCP clients run help as well
    for (bool any : {true, false}) {
        if (!any) lines.insert(lines.end(), {"", "Reserved for users, no LLM/MCP access:"});
        for (CmdInfo const& cmd : GetTraceCommands()) {
            if ((cmd.access == CmdAccess::ANY) != any) continue;
            lines.push_back(std::format("    {} {}", cmd.name, cmd.usage));
            lines.push_back(std::format("        {}", cmd.helptext));
        }
    }
    frontend.showLines(lines, "Worklist Trace Help Menu");
    return {CmdResultCode::SUCCESS_CONTINUE, ""};
}

std::vector<CmdInfo> const TraceCommands = {
    {"collapse",      "[block num]",                                              "Hide IR for block, making it unavailable for annotation", 1, CmdAccess::USER, CmdCollapse},
    {"expand",        "[block num]",                                              "Show IR for block, and make available for annotation", 1, CmdAccess::USER, CmdExpand},
    {"child",         "[block num] [child num]",                                  "Show different predecessor blocks", 2, CmdAccess::ANY, CmdChild},
    {"show",          "[source|ir] [block num]",                                  "Present a preview of IR or a source code approximation of a block", 2, CmdAccess::USER, CmdShow},
    {"toggle-view",   "",                                                         "Toggle whether expanded blocks are shown as IR or a source code approximation", 0, CmdAccess::USER, CmdToggleView},
    {"fp-target",     "[block num] [instr num]",                                  "Annotate possible function pointer target(s)", 2, CmdAccess::USER, CmdFpTarget},
    {"alias-create",  "[yes|no] [block num|function 1] [value 1] [block num|function 2] [value 2]", "Annotate if two values should/should not alias. This automatically creates a new alias group. Values are looked up in the given function, or in the function of the given block.", 5, CmdAccess::ANY, CmdAliasCreate},
    {"alias-add",     "[group num] [block num|function] [value...]",              "Add one or more values to an existing alias group. The values are looked up in the given function, or in the function of the given block.", 3, CmdAccess::ANY, CmdAliasAdd, true},
    {"alias-type",    "[group num] [yes|no]",                                     "Set type of an existing alias group", 2, CmdAccess::ANY, CmdAliasType},
    {"alias-rm",      "[group num] [value num|e<epoch num>...]",                  "Remove values or epochs (prefixed with e) from an existing alias group, by their index in the group (see alias-get)", 2, CmdAccess::ANY, CmdAliasRm, true},
    {"alias-epoch",   "[group num] [block num|function] [instr num|value]",       "Start a new epoch for an alias group before an instruction, given by its number in the block or by its value in the function (an argument: at the start of the function).", 3, CmdAccess::ANY, CmdAliasEpoch},
    {"alias-get",     "[group num...]",                                           "Get info on current alias annotations, of all groups or only of the given ones", 0, CmdAccess::ANY, CmdAliasGet, true},
    {"alias-check",   "[block num|function 1] [value 1] [block num|function 2] [value 2]", "Check whether the analysis takes two pointers as the same, e.g. a parameter of a contract and the address of a store, and why. Uses the current annotations, also those not reanalysed yet.", 4, CmdAccess::ANY, CmdAliasCheck},
    {"alloc-create",  "[block num] [instr num] [size]",                           "Annotate the pointer produced by an instruction as allocated with at least size bytes (0: unknown size).", 3, CmdAccess::ANY, CmdAllocCreate},
    {"reanalyse",     "",                                                         "Re-run all analyses (e.g. after adding annotations)", 0, CmdAccess::USER, CmdReanalyse},
    {"diff",          "[filepath]",                                               "Write out the annotations as a patch file. This includes changes from a previously read patch, if applicable.", 1, CmdAccess::USER, CmdDiff},
    {"patch",         "[filepath]",                                               "Read annotations from a patch file", 1, CmdAccess::USER, CmdPatch},
    {"dump",          "[filepath]",                                               "Write module to file", 1, CmdAccess::USER, CmdDump},
    {"history",       "",                                                         "Print the current commands applied", 0, CmdAccess::ANY, CmdHistory},
    {"chat",          "",                                                         "Ask an LLM about the trace. It can add annotations, which are shown once the chat is closed", 0, CmdAccess::FRONTEND, CmdChat},
    {"settings",      "[llm]",                                                    "Open a settings page. llm: the connection to the LLM server used by the chat", 1, CmdAccess::FRONTEND, CmdSettings},
    {"mcp",           "",                                                         "Let MCP clients inspect the trace and add annotations", 0, CmdAccess::FRONTEND, CmdMCP},
    {"exit",          "",                                                         "Exit the debugger", 0, CmdAccess::USER, CmdExit},
    {"quit",          "",                                                         "Exit the debugger", 0, CmdAccess::USER, CmdExit},
    {"help",          "",                                                         "Show this help text", 0, CmdAccess::ANY, CmdHelp},
};

std::vector<CmdInfo> const& GetTraceCommands() { return TraceCommands; }

// Arguments following the command name, if their number fits the command
std::optional<std::vector<std::string>> SplitArgs(std::string const& input, CmdInfo const& cmd) {
    std::istringstream iss(input);
    std::string arg;
    iss >> arg; // Command name
    std::vector<std::string> args;
    while (iss >> arg) args.push_back(arg);
    if (cmd.variadic ? args.size() < cmd.num_params : args.size() != cmd.num_params) return std::nullopt;
    return args;
}

// The command a command line runs, null if unknown
export CmdInfo const* FindCommand(std::string_view input_command) {
    std::vector<CmdInfo>::const_iterator cmd = std::ranges::find_if(TraceCommands, [&](CmdInfo const& cmd) {
        return input_command == cmd.name || input_command.starts_with(std::format("{} ", cmd.name));
    });
    return cmd == TraceCommands.end() ? nullptr : &*cmd;
}

int running_commands = 0; // Nesting depth of ExecuteCommand

// Whether a command is running. Commands may run event loops (e.g. dialogs), tool calls of MCP clients wait for them.
export bool CommandRunning() { return running_commands > 0; }

CmdResult RunCommandLine(CmdTarget const& target, std::string const& input_command, CmdFrontend const& frontend) {
    CmdInfo const* used_cmd = FindCommand(input_command);
    if (!used_cmd) return {CmdResultCode::UNKNOWN_COMMAND, "Unknown command: " + input_command};
    if (used_cmd->NeedsTrace() && !target.trace) return {CmdResultCode::INVALID_INPUT, std::format("No trace is open, {} needs one", used_cmd->name)};
    std::optional<std::vector<std::string>> args = SplitArgs(input_command, *used_cmd);
    if (!args) return {CmdResultCode::INVALID_SYNTAX, std::format("Invalid syntax. Usage: {} {}", used_cmd->name, used_cmd->usage)};
    running_commands++;
    CmdResult result = used_cmd->NeedsTrace() ? std::get<TraceHandler>(used_cmd->handler)(*args, *target.trace, frontend)
                                              : std::get<ModuleHandler>(used_cmd->handler)(*args, target, frontend);
    running_commands--;
    return result;
}

// Runs a command line, records it in the history, and refreshes the session unless the command ends it
export CmdResult ExecuteCommand(TraceSession& session, std::string const& input_command, CmdFrontend const& frontend) {
    CmdResult result = RunCommandLine({*session.GetModule(), &session}, input_command, frontend);
    session.AddHistory(input_command, result);
    if (result.code != CmdResultCode::SUCCESS_EXIT && result.code != CmdResultCode::SUCCESS_REANALYSE) session.Refresh();
    return result;
}

// Runs a command line while no trace is open, which only commands working on the module do (see CmdInfo::NeedsTrace).
// Recorded in the history like the commands of a trace, the module is written for reanalysing and the patch files
export CmdResult ExecuteModuleCommand(Module& module, std::string const& input_command, CmdFrontend const& frontend) {
    CmdResult result = RunCommandLine({module}, input_command, frontend);
    InteractiveSession::AddToHistory(input_command, result);
    InteractiveSession::WriteReanalyseModule(&module);
    return result;
}

} // namespace InteractiveCmds
