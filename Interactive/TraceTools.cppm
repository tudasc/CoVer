module;

#include <algorithm>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <json/json.h>

#include "ContractTree.hpp"

export module TraceTools;
import AnalysisReports;
import LLVMModule;
import ErrorMessage;
import ContractPassUtility;
import InteractiveUtils;
import InteractiveSession;
import InteractiveCmds;

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

// Null if there is no tool of that name
export template<typename Target>
Tool<Target> const* FindTool(std::vector<Tool<Target>> const& tools, std::string const& name) {
    typename std::vector<Tool<Target>>::const_iterator it = std::ranges::find_if(tools, [&](Tool<Target> const& tool) { return tool.spec.name == name; });
    return it == tools.end() ? nullptr : &*it;
}

export template<typename Target>
std::vector<ToolSpec> SpecsOf(std::vector<Tool<Target>> const& tools) {
    return tools | std::views::transform([](Tool<Target> const& tool) { return tool.spec; }) | std::ranges::to<std::vector>();
}

constexpr size_t MAX_TOOL_OUTPUT = 60000; // Characters
constexpr int MAX_SOURCE_LINES = 400;

// Whether the model may run the command line. Others are run by the user only (see CmdAccess); show_block and get_trace
// provide what the view commands show in a form suited for the model, and set_fp_targets replaces the interactive fp-target.
bool ModelMayRun(std::string const& command) {
    InteractiveCmds::CmdInfo const* cmd = InteractiveCmds::FindCommand(command);
    return !cmd || cmd->access == InteractiveCmds::CmdAccess::ANY; // Unknown commands fail like the user's
}

std::string Truncate(std::string text, size_t max) {
    if (text.size() <= max) return text;
    size_t omitted = text.size() - max;
    text.resize(max);
    return text + std::format("\n[Output truncated, {} more characters omitted]", omitted);
}

// The text without leading and trailing whitespace
export std::string Trim(std::string const& text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

export std::string FirstLine(std::string const& text, size_t max = 160) {
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
- Alias annotations (alias-create, alias-add, alias-type, alias-rm, alias-epoch): when alias analysis is imprecise, parameters of calls cannot be matched (e.g. the request passed to MPI_Wait is not recognised as the one from MPI_Isend). Values in a group are asserted to alias (yes) or not to alias (no). Values are named as in the IR of the function containing the given block (%name, %42, or @global for globals); values defined outside of the block but in the same function can be used as well. An epoch (alias-epoch) placed before an instruction forgets the values of the group seen so far, for memory that is reused over time, such as request arrays reused in every communication round.
- Allocation annotations (alloc-create): the analysis tracks allocated pointers through stores, loads and GEPs, but not through integer arithmetic or opaque calls. Annotating the instruction producing a pointer asserts the pointer refers to memory allocated with at least the given number of bytes (0 if unknown, never more than actually allocated).
New annotations do not affect the current trace, they take effect with the next reanalysis.

# How to work
- Investigate before answering: look at the trace, the relevant blocks and the source code. Do not guess value names or instruction numbers, look them up.
- Explain the root cause of the report in terms of the source code, citing locations as file:line.
- Decide whether the report is a real bug or a false positive. If it is a real bug, say so, suggest a fix to the source code, and do not add annotations to hide it.
- If an annotation is justified, add it, then tell the user exactly which annotations you added and why they are correct. If the user only asked a question, ask before annotating.
- Undo annotations that turn out to be wrong (alias-rm, or set_fp_targets with the previous targets).)";

// Name of the tool returning Context(), not needed when the context is part of the system prompt
export constexpr char const* CONTEXT_TOOL = "get_debug_context";

// Runs the tools on a trace session. Not thread safe, use from the UI thread only.
export class TraceTools {
    public:
        TraceTools(TraceSession& session) : session{session} {}

        TraceSession const& Session() const { return session; }

        // All tools, independent of the session, so they can be listed from any thread
        static std::vector<Tool<TraceTools>> const& Tools() {
            static std::vector<Tool<TraceTools>> const tools = BuildTools();
            return tools;
        }

        ToolResult Run(std::string const& name, Json::Value const& input) {
            Tool<TraceTools> const* tool = FindTool(Tools(), name);
            ToolResult result = tool ? tool->run(*this, input) : ToolResult{std::format("Unknown tool {}", name), true};
            result.text = Truncate(result.text, MAX_TOOL_OUTPUT);
            if (result.summary.empty()) result.summary = result.error ? std::format("{}  ->  {}", name, FirstLine(result.text)) : name;
            return result;
        }

        // The report being debugged, available commands and the current trace
        std::string Context() const {
            std::string out = "# Trace debugger commands\nAvailable through run_command, with the same syntax as the debugger's command line:\n";
            for (InteractiveCmds::CmdInfo const& cmd : InteractiveCmds::GetTraceCommands()) {
                if (cmd.access == InteractiveCmds::CmdAccess::ANY) out += std::format("- {} {}: {}\n", cmd.name, cmd.usage, cmd.helptext);
            }
            AnalysisReports::DebugContext const& ctx = session.Context();
            out += "\n# The report being debugged\n";
            out += std::format("Contract of function {}: {}\nFormula whose trace is shown: {} (status: {})\n", ctx.function, ctx.contract, ctx.formula, ctx.status);
            for (ErrorMessage const& err : ctx.errors) {
                out += "Error report: " + err.text + "\n";
                for (FileReference const& ref : err.references) out += std::format("    Referenced location: {}:{}:{}\n", ref.file, ref.line, ref.column);
            }
            out += "\n# The current trace\n" + TraceOverview();
            return out;
        }

        std::string TraceOverview() const {
            std::vector<TraceBlockView> const& blocks = session.Blocks();
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

    private:
        TraceSession& session;
        std::optional<std::vector<std::string>> source_files; // Files the model may read, from the debug info

        static std::vector<Tool<TraceTools>> BuildTools() {
            Json::Value fp_schema = Schema({{"block", "integer", "Block number"}, {"instruction", "integer", "Instruction number of the indirect call within the block"}, {"functions", "array", "Names of all possible target functions as in the IR"}},
                                           {"block", "instruction", "functions"});
            fp_schema["properties"]["functions"]["items"]["type"] = "string";
            return {
                {{CONTEXT_TOOL, "How to interpret CoVer's trace and annotations, the available debugger commands, the report being debugged and the current trace. Call this first.",
                  Schema({}, {})},
                 [](TraceTools& t, Json::Value const&) -> ToolResult { return {GUIDANCE + "\n\n" + t.Context(), false, CONTEXT_TOOL}; }},
                {{"get_trace", "Overview of the current trace: all blocks with their analysis state, function, source range and children (alternative predecessors). Use it again after switching children.",
                  Schema({}, {})},
                 [](TraceTools& t, Json::Value const&) -> ToolResult { return {t.TraceOverview(), false, "get_trace"}; }},
                {{"show_block", "Instructions of a trace block with their instruction numbers and analysis states, as LLVM IR with source locations or as source code lines.",
                  Schema({{"block", "integer", "Block number"}, {"view", "string", "\"ir\" (default) or \"source\""}}, {"block"})},
                 [](TraceTools& t, Json::Value const& input) { return t.ShowBlock(input); }},
                {{"run_command", "Run one of CoVer's trace debugger commands (listed in the debug context), e.g. \"alias-create yes 0 %5 2 %buf\", \"alloc-create 3 7 1024\", \"child 4 1\" or \"alias-get\". "
                                 "Annotation commands are recorded in the user's command history. Use set_fp_targets instead of fp-target.",
                  Schema({{"command", "string", "The command line"}}, {"command"})},
                 [](TraceTools& t, Json::Value const& input) { return t.RunModelCommand(input); }},
                {{"set_fp_targets", "Annotate the possible targets of an indirect call. Replaces previous targets of the call, an empty list removes the annotation.", fp_schema},
                 [](TraceTools& t, Json::Value const& input) { return t.SetFPTargets(input); }},
                {{"read_source", "Read lines of one of the program's source files. Files are matched by full path or file name.",
                  Schema({{"file", "string", "Path or file name"}, {"first_line", "integer", "First line, 1-based (default 1)"}, {"last_line", "integer", "Last line, inclusive"}}, {"file"})},
                 [](TraceTools& t, Json::Value const& input) { return t.ReadSource(input); }},
                {{"get_function_ir", "LLVM IR of a whole function, e.g. to find value names for alias annotations or to inspect code outside of the trace.",
                  Schema({{"function", "string", "Function name as in the IR (mangled for C++)"}}, {"function"})},
                 [](TraceTools& t, Json::Value const& input) { return t.FunctionIR(input); }},
                {{"list_functions", "Functions of the module, e.g. candidates for function pointer targets.",
                  Schema({{"filter", "string", "Only list functions whose name contains this text"}}, {})},
                 [](TraceTools& t, Json::Value const& input) { return t.ListFunctions(input); }},
            };
        }

        ToolResult ShowBlock(Json::Value const& input) const {
            std::expected<int, ToolResult> block = ParseBlock(input);
            if (!block) return block.error();
            bool source = GetString(input, "view").value_or("ir") == "source";
            return {ShowBlock(*block, source), false, std::format("show_block {} {}", *block, source ? "source" : "ir")};
        }

        ToolResult RunModelCommand(Json::Value const& input) {
            std::optional<std::string> command = GetString(input, "command");
            if (!command) return {"Missing string argument command", true, "run_command"};
            if (!ModelMayRun(*command)) {
                std::string cmd_name = command->substr(0, command->find(' '));
                return {std::format("The command {} is not available to you", cmd_name), true, *command + "  ->  not allowed"};
            }
            return RunCommand(*command);
        }

        // Runs a trace command like the user would, capturing what it would show
        ToolResult RunCommand(std::string const& command, std::vector<std::string> const& selection = {}) {
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
            CmdResult res = InteractiveCmds::ExecuteCommand(session, command, capture);
            bool ok = res.code == CmdResultCode::SUCCESS_CONTINUE;
            std::string text = std::format("{}{}{}", ok ? "" : InteractiveSession::resultCodeStr(res.code) + ": ", res.res, shown.empty() ? "" : "\n" + shown);
            return {text, !ok, std::format("{}  ->  {}", command, res.res.empty() ? (ok ? "done" : InteractiveSession::resultCodeStr(res.code)) : FirstLine(res.res))};
        }

        ToolResult SetFPTargets(Json::Value const& input) {
            std::expected<int, ToolResult> block = ParseBlock(input);
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
            ToolResult result = RunCommand(std::format("fp-target {} {}", *block, *instr), functions);
            if (!result.error) result.summary = std::format("fp-target {} {} [{}]  ->  {}", *block, *instr, names, FirstLine(result.text));
            return result;
        }

        std::expected<int, ToolResult> ParseBlock(Json::Value const& input) const {
            std::optional<int> block = GetInt(input, "block");
            if (!block) return std::unexpected(ToolResult{"Missing integer argument block", true});
            if (*block < 0 || *block >= session.Blocks().size())
                return std::unexpected(ToolResult{std::format("Invalid block {}, must be between 0 and {}", *block, session.Blocks().size() - 1), true});
            return *block;
        }

        std::string ShowBlock(int block, bool source) const {
            TraceBlockView const& view = session.Blocks()[block];
            std::string out = std::format("Block {} in function {}: {}\n", block, view.first.loc->getFunction()->getName().str(), view.description);
            if (source) {
                out += "Source approximation (instruction number of the first instruction of each line, latest executed first):\n";
                for (InteractiveSession::BlockLine const& line : session.BlockLines(block, true)) out += std::format("{:0>3} {}\n", line.instr, line.text);
                return out;
            }
            out += "Instructions (latest executed first):\n";
            ModuleSlotTracker MST(session.GetModule(), false);
            Function const* cur_func = nullptr;
            for (auto const [index, entry] : std::views::enumerate(session.BlockEntries(block))) {
                if (entry.loc->getFunction() != cur_func) {
                    cur_func = entry.loc->getFunction();
                    MST.incorporateFunction(*cur_func);
                }
                std::string ir;
                raw_string_ostream os(ir);
                entry.loc->print(os, MST);
                ir.erase(0, ir.find_first_not_of(' '));
                out += std::format("{:0>3} [{}] {}    ; {}\n", index, entry.info, ir, ContractPassUtility::getInstrLocStr(entry.loc, false));
            }
            return out;
        }

        std::vector<std::string> const& SourceFiles() {
            if (source_files) return *source_files;
            std::set<std::string> files;
            for (Function const& F : *session.GetModule()) {
                for (Instruction const& I : instructions(F)) {
                    if (I.getDebugLoc()) files.insert(ContractPassUtility::getFile(&I));
                }
            }
            source_files = std::vector<std::string>(files.begin(), files.end());
            return *source_files;
        }

        ToolResult ReadSource(Json::Value const& input) {
            std::optional<std::string> file = GetString(input, "file");
            if (!file) return {"Missing string argument file", true, "read_source"};
            std::vector<std::string> const& files = SourceFiles();
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

        ToolResult FunctionIR(Json::Value const& input) {
            std::optional<std::string> name = GetString(input, "function");
            if (!name) return {"Missing string argument function", true, "get_function_ir"};
            Function const* F = session.GetModule()->getFunction(*name);
            if (!F) return {std::format("No function named {} in the module, see list_functions", *name), true, "get_function_ir " + *name};
            std::string ir;
            raw_string_ostream os(ir);
            F->print(os);
            return {ir, false, "get_function_ir " + *name};
        }

        ToolResult ListFunctions(Json::Value const& input) {
            std::string filter = GetString(input, "filter").value_or("");
            std::string out;
            int count = 0;
            for (Function const& F : *session.GetModule()) {
                std::string name = F.getName().str();
                if (name.starts_with("CoVer_") || name.starts_with("llvm.")) continue;
                std::string demangled = demangle(name);
                if (!filter.empty() && name.find(filter) == std::string::npos && demangled.find(filter) == std::string::npos) continue;
                out += std::format("{}{}{}\n", name, demangled != name ? " (" + demangled + ")" : "", F.isDeclaration() ? " [declaration only]" : "");
                count++;
            }
            return {count ? out : "No matching functions", false, std::format("list_functions {}", filter)};
        }
};

// Specs of all tools, independent of the session, so they can be listed from any thread
export std::vector<ToolSpec> const& Specs() {
    static std::vector<ToolSpec> const specs = SpecsOf(TraceTools::Tools());
    return specs;
}

} // namespace TraceTools
