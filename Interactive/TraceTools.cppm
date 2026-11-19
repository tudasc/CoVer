module;

#include <json/json.h>

#include "ContractTree.hpp"

export module TraceTools;
import std;
import AnalysisReports;
import LLVMModule;
import ErrorMessage;
import ContractPassUtility;
import InteractiveUtils;
import InteractiveSession;
import InteractiveCmds;
import AnnotDocs;

using namespace ContractTree;

using namespace llvm;

// Tools for language models working on the trace debugger, shared by the built-in LLM chat and the MCP server.
// Annotations are issued as trace commands, so they end up in the history and patch files like those of the user.
namespace TraceTools {

using InteractiveCmds::CmdFrontend;
using InteractiveSession::CmdResult;
using InteractiveSession::CmdResultCode;
using InteractiveSession::TraceBlockView;
using InteractiveSession::TraceSession;

export struct ToolSpec {
    std::string name;
    std::string description;
    Json::Value schema; // JSON schema of the arguments
};

export struct ToolResult {
    std::string text; // For the model
    bool error = false;
    std::string summary; // One line for the user
};

// A tool and its implementation on Target, kept together so they cannot drift apart
export template<typename Target>
struct Tool {
    ToolSpec spec;
    std::function<ToolResult(Target&, Json::Value const&)> run;
};

// What the trace tools work on, null where not available, e.g. MCP clients have neither module nor trace during the analysis
export struct ToolTarget {
    Module* module = nullptr; // The analysed module
    TraceSession* trace = nullptr; // The trace open in the debugger, of module
};

export ToolTarget TargetOf(TraceSession& trace) {
    return {trace.GetModule(), &trace};
}

// What a trace tool needs of its target
export enum struct ToolNeeds {
    NOTHING,
    MODULE,
    TRACE,
};

// A trace tool, see Tools. It only runs while its target has what it needs, so run can rely on it.
export struct ToolInfo {
    ToolSpec spec;
    ToolNeeds needs;
    std::function<ToolResult(ToolTarget const&, Json::Value const&)> run;
};

// Null if there is no tool of that name. ToolT is a Tool or ToolInfo.
export template<typename ToolT>
ToolT const* FindTool(std::vector<ToolT> const& tools, std::string const& name) {
    typename std::vector<ToolT>::const_iterator it = std::ranges::find_if(tools, [&](ToolT const& tool) { return tool.spec.name == name; });
    return it == tools.end() ? nullptr : &*it;
}

export template<typename ToolT>
std::vector<ToolSpec> SpecsOf(std::vector<ToolT> const& tools) {
    return tools | std::views::transform([](ToolT const& tool) { return tool.spec; }) | std::ranges::to<std::vector>();
}

constexpr std::size_t MAX_TOOL_OUTPUT = 60000; // Characters
constexpr int MAX_SOURCE_LINES = 400;
constexpr int MAX_IR_LINES = 500;

// Whether the model may run the command line. Others are run by the user only (see CmdAccess); show_block and get_trace
// provide what the view commands show in a form suited for the model, and set_fp_targets replaces the interactive fp-target.
bool ModelMayRun(std::string const& command) {
    InteractiveCmds::CmdInfo const* cmd = InteractiveCmds::FindCommand(command);
    return !cmd || cmd->access == InteractiveCmds::CmdAccess::ANY; // Unknown commands fail like the user's
}

// Usage of the commands the model may run. Needs no trace, so it is available before one is opened.
export std::string CommandsSection() {
    std::string out = "# Trace debugger commands\nAvailable through run_command:\n";
    for (InteractiveCmds::CmdInfo const& cmd : InteractiveCmds::GetTraceCommands()) {
        if (cmd.access == InteractiveCmds::CmdAccess::ANY) out += std::format("- {} {}: {}\n", cmd.name, cmd.usage, cmd.helptext);
    }
    return out;
}

std::string Truncate(std::string text, std::size_t max) {
    if (text.size() <= max) return text;
    std::size_t omitted = text.size() - max;
    text.resize(max);
    return text + std::format("\n[Output truncated, {} more characters omitted]", omitted);
}

// The text without leading and trailing whitespace
export std::string Trim(std::string const& text) {
    std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

export std::string FirstLine(std::string const& text, std::size_t max = 160) {
    std::string line = text.substr(0, text.find('\n'));
    if (line.size() > max) line = line.substr(0, max) + "...";
    return line;
}

// JSON schema of an object with the given properties (name, type, description)
export Json::Value Schema(std::vector<std::tuple<std::string, std::string, std::string>> const& props, std::vector<std::string> const& required) {
    Json::Value schema;
    schema["type"] = "object";
    schema["properties"] = Json::objectValue;
    for (auto const& [name, type, description] : props) {
        schema["properties"][name]["type"] = type;
        schema["properties"][name]["description"] = description;
    }
    schema["required"] = Json::arrayValue;
    for (std::string const& name : required) schema["required"].append(name);
    return schema;
}

std::optional<int> GetInt(Json::Value const& input, char const* name) {
    if (!input.isObject() || !input.isMember(name) || !input[name].isInt()) return std::nullopt;
    return input[name].asInt();
}

std::optional<std::string> GetString(Json::Value const& input, char const* name) {
    if (!input.isObject() || !input.isMember(name) || !input[name].isString()) return std::nullopt;
    return input[name].asString();
}

// How to work with the trace debugger, independent of how the model is connected
export std::string const GUIDANCE = R"(CoVer is a static analysis tool that checks whether programs (usually MPI or OpenSHMEM HPC codes) fulfil contracts attached to API functions. The user is inspecting the analysis trace of a reported contract violation in CoVer's trace debugger, and wants to understand the report and, where it is a false positive caused by analysis imprecision, to fix it with annotations. You can inspect the trace, the LLVM IR and the source code, and you can add annotations, using the provided tools.

# The trace
Every contract requirement is checked by a forward dataflow analysis over the interprocedural control flow graph. The trace explains how the analysis reached the state at the error location by walking backwards from it through the predecessors that produced each state.
- The trace is split into blocks. Block 0 is where the trace starts (where the violation is detected); block N+1 executes before block N.
- Within a block, instructions are numbered from 0 and listed in reverse execution order: instruction 0 executes last.
- Instructions are referred to by block number and instruction number, as shown by show_block. Commands taking "[block num] [instr num]" use these numbers.
- If the earliest instruction of a block can be reached from several predecessors ("children"), only the selected child is followed as the next block. The `child` command switches to another child, which renumbers all following blocks, so call get_trace again afterwards.
- FUNCENTRY / FUNCEXIT mark the trace crossing function boundaries, BRANCH / SWITCH mark control flow.
- A block that repeats an earlier one (loop or goto) ends the trace with "then jump to block N".
- Every instruction is labelled with the analysis state the analysis computed there:
  - Pre-call requirements (`PRE { call!(f) }`): starts as NOTCALLED at program entry, becomes CALLED when f is called (PARAMCHECK while the parameters of the call to f still need to match those of the contract supplier), and ERROR when the contract supplier is reached without being CALLED.
  - Post-call requirements (`POST { call!(f) }`): starts as NOTCALLED at the call of the contract supplier and becomes CALLED once f is called. It is violated if the program can end while NOTCALLED.
  - Release requirements (`POST { no! (op) until! (call!(f)) }`): starts as FORBIDDEN at the call of the contract supplier and becomes FULFILLED once f is called. ERROR_UNFULFILLED or ERROR mean the forbidden operation happened while still FORBIDDEN.
  At control flow joins the analysis keeps the worst state, so a single bad path is enough for a report.

# Annotations
Annotations are assumptions the static analysis trusts without checking. They do not change the program's behaviour, but they can make the analysis accept wrong code, so only add one when the program's semantics clearly justify it. They can be checked at runtime with CoVer's annotation verifier.
- Function pointer targets (set_fp_targets tool): the analysis cannot follow indirect calls. Annotating the possible targets of an indirect call lets it analyse them. Annotate all functions the pointer can actually point to, as determined from the source.
- Alias annotations (alias-create, alias-add, alias-type, alias-rm, alias-epoch): when alias analysis is imprecise, parameters of calls cannot be matched (e.g. the request passed to MPI_Wait is not recognised as the one from MPI_Isend). Values in a group are asserted to alias (yes) or not to alias (no). Each value is given by a function and its name in that function's IR (%name, %42, or @global for globals), e.g. `alias-add 0 _Z4sendPi %42`. alias-add takes any number of values of the same function, e.g. `alias-add 0 _Z4sendPi %42 %57 %63`. Instead of the function name, a block number of the trace can be given, which stands for the function containing the block. Prefer function names: block numbers change when another trace is opened or another child is selected, and a value need not be on the trace to be annotated. An epoch (alias-epoch) placed before an instruction forgets the values of the group seen so far, for memory that is reused over time, such as request arrays reused in every communication round. alias-rm removes several members or epochs at once, given as indices and ranges that all refer to the numbering before the removal (`alias-rm 3 0 4-9 e1`). alias-get takes group numbers to show only those groups.
- Allocation annotations (alloc-create): the analysis tracks allocated pointers through stores, loads and GEPs, but not through integer arithmetic or opaque calls. Annotating the instruction producing a pointer asserts the pointer refers to memory allocated with at least the given number of bytes (0 if unknown, never more than actually allocated).
New annotations do not affect the current trace, they take effect with the next reanalysis.
The get_annotation_docs tool returns CoVer's documentation of each annotation kind, with examples and constraints, e.g. how far a source level alias annotation reaches. It shows annotations as calls in the IR or the source code (CoVer_AnnotAlias, ...), which is how they end up in the program. In the trace debugger, add them with the commands above instead. Read the documentation of an annotation kind before using it for the first time.

# How to work
- Investigate before answering: look at the trace, the relevant blocks and the source code. Do not guess value names or instruction numbers, look them up.
- Explain the root cause of the report in terms of the source code, citing locations as file:line.
- Decide whether the report is a real bug or a false positive. If it is a real bug, say so, suggest a fix to the source code, and do not add annotations to hide it.
- If an annotation is justified, add it, then tell the user exactly which annotations you added and why they are correct. If the user only asked a question, ask before annotating.
- Undo annotations that turn out to be wrong (alias-rm, or set_fp_targets with the previous targets).
- The tools run locally in CoVer and are cheap, so make as many tool calls as the task needs: dozens or hundreds of calls and annotations for one report are normal and no problem. Do not skip steps, leave out annotations, or stop to ask the user whether to continue because of the number of calls. Finish the work, then report. Independent tool calls can be made in one response.)";

// Name of the tool returning Context(), not needed when the context is part of the system prompt
export constexpr char const* CONTEXT_TOOL = "get_debug_context";

// Name of the tool returning the annotation documentation, which needs no trace session
export constexpr char const* DOCS_TOOL = "get_annotation_docs";

export ToolSpec DocsSpec() {
    std::string topics;
    for (AnnotDocs::Doc const& doc : AnnotDocs::Docs()) topics += (topics.empty() ? "" : ", ") + doc.topic;
    Json::Value schema = Schema({{"topic", "string", std::format("One of {}, all if omitted", topics)}}, {});
    for (AnnotDocs::Doc const& doc : AnnotDocs::Docs()) schema["properties"]["topic"]["enum"].append(doc.topic);
    return {DOCS_TOOL, "CoVer's documentation of an annotation kind: what it asserts, examples in IR and source code, and its constraints.", schema};
}

export ToolResult AnnotationDocs(Json::Value const& input) {
    std::optional<std::string> topic = GetString(input, "topic");
    std::string out;
    for (AnnotDocs::Doc const& doc : AnnotDocs::Docs()) {
        if (!topic || *topic == doc.topic) out += std::string(doc.text) + "\n";
    }
    if (out.empty()) return {std::format("Unknown topic {}, see the tool description", *topic), true, std::format("{} {}  ->  unknown topic", DOCS_TOOL, *topic)};
    return {out, false, std::format("{} {}", DOCS_TOOL, topic.value_or("(all)"))};
}

// A line of the IR shown by get_function_ir, with its instruction if it is one
struct IRLine {
    std::string text;
    Instruction const* instr;
};

template<typename Range>
std::string Join(Range const& parts, std::string_view separator) {
    return parts | std::views::join_with(separator) | std::ranges::to<std::string>();
}

// Names of the source variables the debug records of F describe, by the value holding them: allocas in unoptimised code, SSA values and arguments in optimised code
std::map<Value const*, std::set<std::string>> VariableNames(Function const& F) {
    std::map<Value const*, std::set<std::string>> names;
    for (Instruction const& I : instructions(F)) {
        for (DbgRecord const& DR : I.getDbgRecordRange()) {
            DbgVariableRecord const* DVR = dyn_cast<DbgVariableRecord>(&DR);
            if (!DVR || DVR->getVariable()->getName().empty()) continue;
            for (Value const* V : DVR->location_ops()) {
                if (V) names[V].insert(DVR->getVariable()->getName().str());
            }
        }
    }
    return names;
}

// Comment for the definition of F naming the source variables its arguments hold, empty if none
std::string ArgumentVariables(Function const& F, std::map<Value const*, std::set<std::string>> const& variables) {
    std::vector<std::string> args;
    for (Argument const& A : F.args()) {
        std::map<Value const*, std::set<std::string>>::const_iterator var = variables.find(&A);
        if (var != variables.end()) args.push_back(std::format("{} {}", ContractPassUtility::getValueName(&A), Join(var->second, "/")));
    }
    return args.empty() ? "" : "    ; variables " + Join(args, ", ");
}

// Source location of an instruction for get_function_ir: the line, and the file if it is not the function's
std::string LocationNote(Instruction const* I, std::string const& function_file) {
    if (!I->getDebugLoc()) return "no location";
    std::string file = ContractPassUtility::getFile(I, false);
    return file == function_file ? std::format("line {}", I->getDebugLoc().getLine()) : std::format("{}:{}", file, I->getDebugLoc().getLine());
}

// Marks the instruction lines of a printed function, so they can be filtered and given their source location
class InstructionMarker : public AssemblyAnnotationWriter {
    public:
        static constexpr char MARK = '\x1f'; // Printed IR escapes control characters in strings, so it only occurs as the marker

        std::vector<Instruction const*> instructions; // In the order of the marked lines

        void printInfoComment(Value const& V, formatted_raw_ostream& os) override {
            if (Instruction const* I = dyn_cast<Instruction>(&V)) {
                os << MARK;
                instructions.push_back(I);
            }
        }
};

// The tools below run on the UI thread only, they are not thread safe

export std::vector<ToolInfo> const& Tools();

export bool Available(ToolInfo const& tool, ToolTarget const& target) {
    switch (tool.needs) {
        case ToolNeeds::NOTHING: return true;
        case ToolNeeds::MODULE: return target.module != nullptr;
        case ToolNeeds::TRACE: return target.trace != nullptr;
    }
    return false;
}

// Why a tool is not available on target, with the tools that are. Only while target has no trace.
std::string Unavailable(ToolTarget const& target) {
    std::string available;
    for (ToolInfo const& tool : Tools()) {
        if (Available(tool, target)) available += (available.empty() ? "" : ", ") + tool.spec.name;
    }
    return std::format("{} Available now: {}.", target.module ? "No trace is open, open a report first." : "The analysis is still running, neither the module nor a trace is available.", available);
}

std::string TraceOverview(TraceSession const& trace) {
    std::vector<TraceBlockView> const& blocks = trace.Blocks();
    std::string out = std::format("The trace has {} blocks.\n", blocks.size());
    for (TraceBlockView const& block : blocks) {
        out += std::format("Block {} [{}] in function {}: {}\n", block.index, block.first.info, block.first.loc->getFunction()->getName().str(), block.description);
        if (block.children.size() <= 1) continue;
        for (auto const [child, entry] : std::views::enumerate(block.children)) {
            out += std::format("    child {} at {} [{}]{}\n", child, ContractPassUtility::getInstrLocStr(entry.loc, false), entry.info,
                               child == block.selected_child ? std::format(" (selected, shown as block {})", block.index + 1) : "");
        }
    }
    return out;
}

// The report being debugged, available commands and the current trace
export std::string Context(ToolTarget const& target) {
    std::string out = CommandsSection();
    if (!target.trace) return out + "\n# The current trace\n" + Unavailable(target) + "\n";
    AnalysisReports::DebugContext const& ctx = target.trace->Context();
    out += "\n# The report being debugged\n";
    out += std::format("Contract of function {}: {}\nFormula whose trace is shown: {} (status: {})\n", ctx.function, ctx.contract, ctx.formula, ctx.status);
    for (ErrorMessage const& err : ctx.errors) {
        out += "Error report: " + err.text + "\n";
        for (FileReference const& ref : err.references) out += std::format("    Referenced location: {}:{}:{}\n", ref.file, ref.line, ref.column);
    }
    out += "\n# The current trace\n" + TraceOverview(*target.trace);
    return out;
}

// Runs a trace command like the user would, capturing what it would show
ToolResult RunCommand(TraceSession& trace, std::string const& command, std::vector<std::string> const& selection = {}) {
    std::string shown;
    CmdFrontend capture = {
        .showLines = [&](std::vector<std::string> const& lines, std::string const& title) {
            shown += title + ":\n";
            for (std::string const& line : lines) shown += line + "\n";
        },
        .selectMultiple = [&](std::vector<std::string> const&, std::string const&, std::set<std::string> const&) { return selection; },
        .showHistory = [&](InteractiveSession::History const& history) {
            for (auto const& [cmd, res] : history) shown += std::format("{} -> {}\n", cmd, InteractiveSession::resultCodeStr(res.code));
        },
    };
    CmdResult res = InteractiveCmds::ExecuteCommand(trace, command, capture);
    bool ok = res.code == CmdResultCode::SUCCESS_CONTINUE;
    std::string text = std::format("{}{}{}", ok ? "" : InteractiveSession::resultCodeStr(res.code) + ": ", res.res, shown.empty() ? "" : "\n" + shown);
    return {text, !ok, std::format("{}  ->  {}", command, res.res.empty() ? (ok ? "done" : InteractiveSession::resultCodeStr(res.code)) : FirstLine(res.res))};
}

ToolResult RunModelCommand(TraceSession& trace, Json::Value const& input) {
    std::optional<std::string> command = GetString(input, "command");
    if (!command) return {"Missing string argument command", true, "run_command"};
    if (!ModelMayRun(*command)) {
        std::string cmd_name = command->substr(0, command->find(' '));
        return {std::format("The command {} is not available to you, help lists the commands that are", cmd_name), true, *command + "  ->  not allowed"};
    }
    return RunCommand(trace, *command);
}

std::expected<int, ToolResult> ParseBlock(TraceSession const& trace, Json::Value const& input) {
    std::optional<int> block = GetInt(input, "block");
    if (!block) return std::unexpected(ToolResult{"Missing integer argument block", true});
    if (*block < 0 || *block >= trace.Blocks().size())
        return std::unexpected(ToolResult{std::format("Invalid block {}, must be between 0 and {}", *block, trace.Blocks().size() - 1), true});
    return *block;
}

ToolResult SetFPTargets(TraceSession& trace, Json::Value const& input) {
    std::expected<int, ToolResult> block = ParseBlock(trace, input);
    if (!block) return block.error();
    std::optional<int> instr = GetInt(input, "instruction");
    if (!instr || !input["functions"].isArray()) return {"Missing argument instruction or functions", true, "set_fp_targets"};
    std::vector<std::string> functions;
    std::string names;
    for (Json::Value const& F : input["functions"]) {
        if (!F.isString()) return {"The functions must be strings", true, "set_fp_targets"};
        functions.push_back(F.asString());
        names += (names.empty() ? "" : ", ") + F.asString();
    }
    ToolResult result = RunCommand(trace, std::format("fp-target {} {}", *block, *instr), functions);
    if (!result.error) result.summary = std::format("fp-target {} {} [{}]  ->  {}", *block, *instr, names, FirstLine(result.text));
    return result;
}

std::string ShowBlock(TraceSession const& trace, int block, bool source) {
    TraceBlockView const& view = trace.Blocks()[block];
    std::string out = std::format("Block {} in function {}: {}\n", block, view.first.loc->getFunction()->getName().str(), view.description);
    if (source) {
        out += "Source approximation (instruction number of the first instruction of each line, latest executed first):\n";
        for (InteractiveSession::BlockLine const& line : trace.BlockLines(block, true)) out += std::format("{:0>3} {}\n", line.instr, line.text);
        return out;
    }
    out += "Instructions (latest executed first):\n";
    ModuleSlotTracker MST(trace.GetModule(), false);
    Function const* cur_func = nullptr;
    for (auto const [index, entry] : std::views::enumerate(trace.BlockEntries(block))) {
        if (entry.loc->getFunction() != cur_func) {
            cur_func = entry.loc->getFunction();
            MST.incorporateFunction(*cur_func);
        }
        std::string ir;
        raw_string_ostream os(ir);
        entry.loc->print(os, MST);
        ir.erase(0, ir.find_first_not_of(' '));
        std::string location = entry.loc->getDebugLoc() ? "    ; " + ContractPassUtility::getInstrLocStr(entry.loc, false) : "";
        out += std::format("{:0>3} [{}] {}{}\n", index, entry.info, InteractiveUtils::CompactIR(ir), location);
    }
    return out;
}

ToolResult ShowBlock(TraceSession const& trace, Json::Value const& input) {
    std::expected<int, ToolResult> block = ParseBlock(trace, input);
    if (!block) return block.error();
    bool source = GetString(input, "view").value_or("ir") == "source";
    return {ShowBlock(trace, *block, source), false, std::format("show_block {} {}", *block, source ? "source" : "ir")};
}

// Files the model may read, from the debug info. Cached for the last module, the analysed one stays while CoVer runs.
std::vector<std::string> const& SourceFiles(Module const& module) {
    static Module const* cached = nullptr;
    static std::vector<std::string> source_files;
    if (cached == &module) return source_files;
    std::set<std::string> files;
    for (Function const& F : module) {
        for (Instruction const& I : instructions(F)) {
            if (I.getDebugLoc()) files.insert(ContractPassUtility::getFile(&I));
        }
    }
    cached = &module;
    source_files = std::vector<std::string>(files.begin(), files.end());
    return source_files;
}

ToolResult ReadSource(Module const& module, Json::Value const& input) {
    std::optional<std::string> file = GetString(input, "file");
    if (!file) return {"Missing string argument file", true, "read_source"};
    std::vector<std::string> const& files = SourceFiles(module);
    std::vector<std::string> matches;
    for (std::string const& f : files) {
        if (f == *file || std::filesystem::path(f).filename() == *file || f.ends_with("/" + *file)) matches.push_back(f);
    }
    if (matches.size() != 1) {
        std::string list;
        for (std::string const& f : files) list += f + "\n";
        return {std::format("{} the source files of the program:\n{}", matches.empty() ? "File is not one of" : "Ambiguous file name, it matches several of", list), true, "read_source " + *file};
    }
    std::ifstream stream(matches[0]);
    if (!stream) return {"Could not open " + matches[0], true, "read_source " + *file};
    std::vector<std::string> lines;
    for (std::string line; std::getline(stream, line);) lines.push_back(line);
    int first = std::max(1, GetInt(input, "first_line").value_or(1));
    int last = std::min<int>(lines.size(), GetInt(input, "last_line").value_or(first + MAX_SOURCE_LINES - 1));
    if (first > last) return {std::format("Invalid line range, the file has {} lines", lines.size()), true, "read_source " + *file};
    std::string out = std::format("{} lines {}-{} of {}:\n", matches[0], first, std::min(last, first + MAX_SOURCE_LINES - 1), lines.size());
    for (int line = first; line <= last && line < first + MAX_SOURCE_LINES; line++) out += std::format("{:>5} | {}\n", line, lines[line - 1]);
    if (last >= first + MAX_SOURCE_LINES) out += std::format("[Stopped after {} lines, request the rest separately]\n", MAX_SOURCE_LINES);
    return {out, false, std::format("read_source {}:{}-{}", std::filesystem::path(matches[0]).filename().string(), first, last)};
}

ToolResult FunctionIR(Module const& module, Json::Value const& input) {
    std::optional<std::string> name = GetString(input, "function");
    if (!name) return {"Missing string argument function", true, "get_function_ir"};
    Function const* F = module.getFunction(*name);
    if (!F) return {std::format("No function named {} in the module, see list_functions", *name), true, "get_function_ir " + *name};
    std::optional<int> first_line = GetInt(input, "first_line");
    std::optional<int> last_line = GetInt(input, "last_line");
    bool filtered = first_line || last_line;

    std::string function_file = F->getSubprogram() ? F->getSubprogram()->getFilename().str() : "";
    std::map<Value const*, std::set<std::string>> variables = VariableNames(*F);

    // Printed by LLVM, so the value names are those the commands take
    InstructionMarker marker;
    std::string printed;
    raw_string_ostream os(printed);
    F->print(os, &marker);
    std::vector<IRLine> lines;
    std::string label; // Of the current basic block, shown before its first instruction in the line range
    int instr = 0;
    for (std::size_t pos = 0, next; pos < printed.size(); pos = next + 1) {
        next = std::min(printed.find('\n', pos), printed.size());
        std::string_view line = std::string_view(printed).substr(pos, next - pos);
        std::size_t mark = line.find(InstructionMarker::MARK);
        if (mark == std::string_view::npos) {
            // Debug records are left out, the comments name the variables they describe
            if (line.starts_with("; Function Attrs:") || line.substr(std::min(line.find_first_not_of(' '), line.size())).starts_with("#dbg_")) continue;
            if (!filtered) lines.push_back({InteractiveUtils::CompactIR(line) + (line.starts_with("define ") ? ArgumentVariables(*F, variables) : ""), nullptr});
            else if (line.find(':') < line.find(' ')) label = InteractiveUtils::CompactIR(line); // A label like "12:", followed by the predecessors
            continue;
        }
        Instruction const* I = marker.instructions[instr++];
        if (ContractPassUtility::isRemovedAnnot(I)) continue; // Hidden like in the trace, erased once CoVer finishes
        int src_line = I->getDebugLoc() ? I->getDebugLoc().getLine() : 0;
        if (filtered && (src_line < first_line.value_or(0) || src_line > last_line.value_or(std::numeric_limits<int>::max()))) continue;
        if (!label.empty()) lines.push_back({std::exchange(label, ""), nullptr});
        lines.push_back({InteractiveUtils::CompactIR(line.substr(0, mark)), I});
    }

    int num_lines = lines.size();
    int first = std::max(0, GetInt(input, "offset").value_or(0));
    if (first >= num_lines) return {std::format("No lines from offset {}, the output has {} lines", first, num_lines), true, "get_function_ir " + *name};
    int last = std::min(num_lines, first + MAX_IR_LINES);
    std::string out = std::format("IR of {}, lines {}-{} of {}. Source locations{} are given where they change, the following instructions have the same location. "
                                  "Values holding source variables are named after them.\n",
                                  F->getName().str(), first, last - 1, num_lines, function_file.empty() ? "" : std::format(" (in {} unless another file is given)", function_file));
    std::string shown_location; // Given last on this page, so that the first instruction of each page has its location
    for (IRLine const& line : lines | std::views::drop(first) | std::views::take(last - first)) {
        std::vector<std::string> notes;
        if (line.instr) {
            std::string location = LocationNote(line.instr, function_file);
            if (location != shown_location) notes.push_back(shown_location = location);
            std::map<Value const*, std::set<std::string>>::const_iterator var = variables.find(line.instr);
            if (var != variables.end()) notes.push_back("variable " + Join(var->second, "/"));
        }
        out += notes.empty() ? line.text + "\n" : std::format("{}    ; {}\n", line.text, Join(notes, ", "));
    }
    if (last < num_lines) out += std::format("[Stopped after {} lines, call again with offset {} for the rest]\n", MAX_IR_LINES, last);
    return {out, false, std::format("get_function_ir {} from {}", *name, first)};
}

ToolResult ListFunctions(Module const& module, Json::Value const& input) {
    std::string filter = GetString(input, "filter").value_or("");
    std::string out;
    int count = 0;
    for (Function const& F : module) {
        std::string name = F.getName().str();
        if (name.starts_with("CoVer_") || name.starts_with("llvm.")) continue;
        std::string demangled = demangle(name);
        if (!filter.empty() && name.find(filter) == std::string::npos && demangled.find(filter) == std::string::npos) continue;
        out += std::format("{}{}{}\n", name, demangled != name ? " (" + demangled + ")" : "", F.isDeclaration() ? " [declaration only]" : "");
        count++;
    }
    return {count ? out : "No matching functions", false, std::format("list_functions {}", filter)};
}

std::vector<ToolInfo> BuildTools() {
    Json::Value fp_schema = Schema({{"block", "integer", "Block number"}, {"instruction", "integer", "Instruction number of the indirect call within the block"}, {"functions", "array", "Names of all possible target functions as in the IR"}},
                                   {"block", "instruction", "functions"});
    fp_schema["properties"]["functions"]["items"]["type"] = "string";
    return {
        {{CONTEXT_TOOL, "How to interpret CoVer's trace and annotations, the available debugger commands, the report being debugged and the current trace. Call this first.",
          Schema({}, {})},
         ToolNeeds::NOTHING, [](ToolTarget const& t, Json::Value const&) -> ToolResult { return {GUIDANCE + "\n\n" + Context(t), false, CONTEXT_TOOL}; }},
        {{"get_trace", "Overview of the current trace: all blocks with their analysis state, function, source range and children (alternative predecessors). Use it again after switching children.",
          Schema({}, {})},
         ToolNeeds::TRACE, [](ToolTarget const& t, Json::Value const&) -> ToolResult { return {TraceOverview(*t.trace), false, "get_trace"}; }},
        {{"show_block", "Instructions of a trace block with their instruction numbers and analysis states, as LLVM IR with source locations or as source code lines.",
          Schema({{"block", "integer", "Block number"}, {"view", "string", "\"ir\" (default) or \"source\""}}, {"block"})},
         ToolNeeds::TRACE, [](ToolTarget const& t, Json::Value const& input) { return ShowBlock(*t.trace, input); }},
        {{"run_command", "Run one of CoVer's trace debugger commands (listed in the debug context), e.g. \"alias-create yes main %5 _Z4sendPi %buf\", \"alias-add 0 main %7 %9 %12\", \"alloc-create 3 7 1024\", \"child 4 1\" or \"alias-get\". "
                         "Commands run locally and are cheap, run as many as needed. Annotation commands are recorded in the user's command history. Use set_fp_targets instead of fp-target.",
          Schema({{"command", "string", "The command line"}}, {"command"})},
         ToolNeeds::TRACE, [](ToolTarget const& t, Json::Value const& input) { return RunModelCommand(*t.trace, input); }},
        {{"set_fp_targets", "Annotate the possible targets of an indirect call. Replaces previous targets of the call, an empty list removes the annotation.", fp_schema},
         ToolNeeds::TRACE, [](ToolTarget const& t, Json::Value const& input) { return SetFPTargets(*t.trace, input); }},
        {{"read_source", "Read lines of one of the program's source files. Files are matched by full path or file name.",
          Schema({{"file", "string", "Path or file name"}, {"first_line", "integer", "First line, 1-based (default 1)"}, {"last_line", "integer", "Last line, inclusive"}}, {"file"})},
         ToolNeeds::MODULE, [](ToolTarget const& t, Json::Value const& input) { return ReadSource(*t.module, input); }},
        {{"get_function_ir", "LLVM IR of a function with source locations and the source variables values hold, e.g. to find value names for alias annotations or to inspect code outside of the trace. "
                             "Long functions are returned in parts (see offset); restrict the output to a range of source lines to find values quickly.",
          Schema({{"function", "string", "Function name as in the IR (mangled for C++)"},
                  {"first_line", "integer", "Only instructions of this source line and later"},
                  {"last_line", "integer", "Only instructions up to this source line, inclusive"},
                  {"offset", "integer", "First line of the output to return, to continue a long output (default 0)"}}, {"function"})},
         ToolNeeds::MODULE, [](ToolTarget const& t, Json::Value const& input) { return FunctionIR(*t.module, input); }},
        {{"list_functions", "Functions of the module, e.g. candidates for function pointer targets.",
          Schema({{"filter", "string", "Only list functions whose name contains this text"}}, {})},
         ToolNeeds::MODULE, [](ToolTarget const& t, Json::Value const& input) { return ListFunctions(*t.module, input); }},
        {DocsSpec(),
         ToolNeeds::NOTHING, [](ToolTarget const&, Json::Value const& input) { return AnnotationDocs(input); }},
    };
}

// All tools, independent of the target, so they can be listed from any thread
std::vector<ToolInfo> const& Tools() {
    static std::vector<ToolInfo> const tools = BuildTools();
    return tools;
}

// Runs the tool on target, if it has what the tool needs
export ToolResult Run(std::string const& name, Json::Value const& input, ToolTarget const& target) {
    ToolInfo const* tool = FindTool(Tools(), name);
    ToolResult result = !tool ? ToolResult{std::format("Unknown tool {}", name), true}
                      : !Available(*tool, target) ? ToolResult{Unavailable(target), true, std::format("{}  ->  {}", name, target.module ? "no trace open" : "analysis running")}
                      : tool->run(target, input);
    result.text = Truncate(result.text, MAX_TOOL_OUTPUT);
    if (result.summary.empty()) result.summary = result.error ? std::format("{}  ->  {}", name, FirstLine(result.text)) : name;
    return result;
}

// Specs of all tools, independent of the target, so they can be listed from any thread
export std::vector<ToolSpec> const& Specs() {
    static std::vector<ToolSpec> const specs = SpecsOf(Tools());
    return specs;
}

} // namespace TraceTools
