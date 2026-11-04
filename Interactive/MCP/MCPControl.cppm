module;

#include <algorithm>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <json/json.h>

#include "../../Passes/ContractManager.hpp"
#include "ContractTree.hpp"

export module MCPControl;
import ErrorMessage;
import InteractiveUtils;
import InteractiveSession;
import InteractiveCmds;
import TraceTools;
import MCPServer;
import LLVMModule;

using namespace llvm;

// MCP for the whole interactive session: owns the server, and dispatches tool calls depending on what is shown.
// Besides the trace tools, MCP clients can list the reports, open their traces and reanalyse, so they can work through all reports.
// The frontends declare what is shown with the scopes below and perform the requested navigation (OpenPendingReport, NavigateTrace).
namespace MCPControl {

using InteractiveUtils::PostFn;
using InteractiveUtils::ReportedFormula;
using MCPServer::Activity;
using TraceTools::ToolResult;
using TraceTools::ToolSpec;

// Navigation requested by a tool call, performed by the frontend
export struct Action {
    enum struct Kind { NONE, OPEN_REPORT, REANALYSE };
    Kind kind = Kind::NONE;
    int report = -1;
    int leaf = -1; // Index among the report's DebuggableLeaves
};

std::string const WORKFLOW = R"(# Working through the reports
You are connected to CoVer's interactive mode through MCP and can work through all reports by yourself:
- list_reports lists the reports of the current analysis. A report can have several traces, one per violated leaf of its formula, listed with their leaf index.
  open_report opens the trace of a leaf in the trace debugger, the other tools work on the open trace. Look at all traces of a report: each leaf may be violated for a different reason.
- Annotations are kept when you open another report, so you can annotate several reports before reanalysing.
- reanalyse reruns the analysis with all annotations added so far. CoVer restarts for this: wait a few seconds before the next call, and wait and retry while the server is unreachable or reports that CoVer is restarting. If the leaf whose trace was open is still reported afterwards, its trace is opened again automatically.
- After reanalysing, check with list_reports that the reports you annotated are gone. Annotations can also uncover further reports.
- Reports without a debuggable trace (marked in list_reports) cannot be annotated with these tools, leave them to the user.
- Annotate false positives only. Leave real bugs reported, and tell the user about them with a suggested fix.
- When done, tell the user which annotations you added for which reports, and which reports are real bugs. The user can save the annotations as a patch file with the diff command of CoVer's debugger.)";

// Line appended to the history cache if MCP mode was active when reanalysing, followed by the key of the report being debugged
constexpr std::string_view RESUME_PREFIX = "|| MCP RESUME || ";

// A screen of the frontend able to run tool calls, see ScreenScope
struct Screen {
    std::function<void(std::vector<Activity> const&)> after;
};

struct State {
    std::unique_ptr<MCPServer::Server> server;
    bool active = false; // MCP mode: tool calls are run, see Start and Leave
    std::vector<Activity> log;
    Action pending;
    std::vector<ReportedFormula> const* results = nullptr; // Of the current analysis, while the results are shown
    ContractManagerAnalysis::FormulaReports const* result_texts = nullptr;
    ContractManagerAnalysis::FormulaDebugHandlers* result_debug_handlers = nullptr;
    TraceTools::TraceTools* trace = nullptr; // While a trace is open
    std::vector<Screen> screens; // Innermost last
    std::optional<std::string> resume; // Key of the trace to open again once the results of a reanalysis are shown, see TraceKey
};

State state;

// Violated leaves of the formula with a trace that can be debugged, in depth-first order. MCP clients address them by their index.
void CollectLeaves(std::shared_ptr<ContractTree::ContractFormula> const& form, ContractManagerAnalysis::FormulaDebugHandlers const& debugHandlers,
                   std::vector<std::shared_ptr<ContractTree::ContractExpression>>& leaves) {
    if (*form->Status != ContractTree::Fulfillment::FULFILLED && debugHandlers.contains(form.get())) leaves.push_back(std::static_pointer_cast<ContractTree::ContractExpression>(form));
    for (std::shared_ptr<ContractTree::ContractFormula> const& child : form->Children) CollectLeaves(child, debugHandlers, leaves);
}

std::vector<std::shared_ptr<ContractTree::ContractExpression>> DebuggableLeaves(ReportedFormula const& report, ContractManagerAnalysis::FormulaDebugHandlers const& debugHandlers) {
    std::vector<std::shared_ptr<ContractTree::ContractExpression>> leaves;
    CollectLeaves(report.formula, debugHandlers, leaves);
    return leaves;
}

// Identifies a trace across analysis runs: one is reported per contract and top level formula, and has a trace per violated leaf.
// Leaf indices may change when a reanalysis fixes other leaves, so the leaf's formula is used.
std::string TraceKey(std::string const& function, std::string const& report, std::string const& leaf) {
    return function + "|" + report + "|" + leaf;
}

std::string TraceKey(ReportedFormula const& report, ContractTree::ContractFormula const& leaf) {
    return TraceKey(report.contract->F->getName().str(), report.formula->ExprStr, leaf.ExprStr);
}

std::string TraceKey(ContractManagerAnalysis::DebugContext const& context) {
    return TraceKey(context.function, context.report, context.formula);
}

std::string OpenTraceKey(State const& s) {
    return s.trace ? TraceKey(s.trace->Session().Context()) : "";
}

std::string ReportsText(State const& s) {
    if (!s.results) return "# Reports\nNot available yet, the analysis is still running.\n";
    std::string open_key = OpenTraceKey(s);
    std::string out = std::format("# Reports\n{} reports.\n", s.results->size());
    for (int i = 0; i < s.results->size(); i++) {
        ReportedFormula const& report = (*s.results)[i];
        std::vector<std::shared_ptr<ContractTree::ContractExpression>> leaves = DebuggableLeaves(report, *s.result_debug_handlers);
        out += std::format("Report {}: {}{}\n", i, report.Summary(), leaves.empty() ? " [no debuggable trace]" : "");
        out += std::format("    Formula: {} ({})\n", report.formula->ExprStr, ContractTree::FulfillmentStr(*report.formula->Status));
        if (s.result_texts && s.result_texts->contains(report.formula.get())) {
            for (ErrorMessage const& err : s.result_texts->at(report.formula.get())) {
                out += "    Error: " + err.text + "\n";
                for (FileReference const& ref : err.references) out += std::format("    At {}:{}:{}\n", ref.file, ref.line, ref.column);
            }
        }
        for (int leaf = 0; leaf < leaves.size(); leaf++) {
            bool open = !open_key.empty() && TraceKey(report, *leaves[leaf]) == open_key;
            out += std::format("    Leaf {}: {} ({}){}\n", leaf, leaves[leaf]->ExprStr, ContractTree::FulfillmentStr(*leaves[leaf]->Status), open ? " [trace open]" : "");
        }
    }
    return out;
}

ToolResult OpenReport(State& s, Json::Value const& args) {
    if (!s.results) return {"The analysis is still running", true, "open_report  ->  analysis running"};
    if (!args.isObject() || !args["report"].isInt() || !args["leaf"].isInt()) return {"Missing integer arguments report and leaf", true, "open_report"};
    int idx = args["report"].asInt();
    int leaf = args["leaf"].asInt();
    if (idx < 0 || idx >= s.results->size()) return {std::format("Invalid report {}, there are {} reports", idx, s.results->size()), true, std::format("open_report {} {}  ->  invalid report", idx, leaf)};
    ReportedFormula const& report = (*s.results)[idx];
    std::vector<std::shared_ptr<ContractTree::ContractExpression>> leaves = DebuggableLeaves(report, *s.result_debug_handlers);
    if (leaves.empty()) return {std::format("Report {} has no debuggable trace", idx), true, std::format("open_report {} {}  ->  no debuggable trace", idx, leaf)};
    if (leaf < 0 || leaf >= leaves.size())
        return {std::format("Invalid leaf {}, report {} has {} leaves with a trace, see list_reports", leaf, idx, leaves.size()), true, std::format("open_report {} {}  ->  invalid leaf", idx, leaf)};
    if (TraceKey(report, *leaves[leaf]) == OpenTraceKey(s))
        return {std::format("The trace of leaf {} of report {} is already open", leaf, idx), false, std::format("open_report {} {}  ->  already open", idx, leaf)};
    s.pending = {Action::Kind::OPEN_REPORT, idx, leaf};
    return {std::format("Opening the trace of leaf {} of report {} ({}), use get_trace to inspect it", leaf, idx, leaves[leaf]->ExprStr), false, std::format("open_report {} {}", idx, leaf)};
}

ToolResult Reanalyse(State& s, Json::Value const&) {
    if (!s.trace) return {"Reanalysis is started from the trace debugger, open a report first", true, "reanalyse  ->  no trace open"};
    s.pending = {Action::Kind::REANALYSE};
    s.server->SetAccepting(false, MCPServer::RESTARTING); // Calls until this process exits would get lost
    return {"Reanalysis started. CoVer restarts: wait a few seconds, then call list_reports, retrying while the server is unreachable or reports that CoVer is restarting.", false, "reanalyse"};
}

// The trace tools, working on the open trace, followed by the navigation tools
std::vector<TraceTools::Tool<State>> BuildTools() {
    std::vector<TraceTools::Tool<State>> tools;
    for (ToolSpec const& spec : TraceTools::Specs()) {
        if (spec.name == TraceTools::CONTEXT_TOOL) {
            tools.push_back({spec, [](State& s, Json::Value const&) -> ToolResult {
                std::string trace = s.trace ? s.trace->Context() : "# The current trace\nNo trace is open, use open_report.\n";
                return {TraceTools::GUIDANCE + "\n\n" + WORKFLOW + "\n\n" + trace + "\n" + ReportsText(s), false, TraceTools::CONTEXT_TOOL};
            }});
            continue;
        }
        tools.push_back({spec, [name = spec.name](State& s, Json::Value const& args) -> ToolResult {
            if (!s.trace) return {"No trace is open, use list_reports and open_report first", true, name + "  ->  no trace open"};
            return s.trace->Run(name, args);
        }});
    }
    Json::Value open_schema = TraceTools::Schema({{"report", "integer", "Report number, see list_reports"}, {"leaf", "integer", "Index of the violated leaf whose trace to open, see list_reports"}}, {"report", "leaf"});
    tools.push_back({{"list_reports", "Reports of the current analysis with their number, contract, message, error report and locations, and the violated leaves of their formula that have a trace, with their index and whose trace is open.", TraceTools::Schema({}, {})},
                     [](State& s, Json::Value const&) -> ToolResult { return {ReportsText(s), false, "list_reports"}; }});
    tools.push_back({{"open_report", "Open the trace of a violated leaf of a report in the trace debugger, closing the currently open trace. Annotations added so far are kept.", open_schema}, OpenReport});
    tools.push_back({{"reanalyse", "Rerun the analysis with all annotations added so far, possible while a trace is open. CoVer restarts, so the server is unreachable for a few seconds.", TraceTools::Schema({}, {})}, Reanalyse});
    return tools;
}

std::vector<TraceTools::Tool<State>> const& Tools() {
    static std::vector<TraceTools::Tool<State>> const tools = BuildTools();
    return tools;
}

ToolResult Run(std::string const& name, Json::Value const& args) {
    if (state.pending.kind != Action::Kind::NONE)
        return {std::format("CoVer is {}, wait a moment and then continue", state.pending.kind == Action::Kind::REANALYSE ? "reanalysing" : "opening a report"), true, name + "  ->  busy"};
    TraceTools::Tool<State> const* tool = TraceTools::FindTool(Tools(), name);
    if (!tool) return {"Unknown tool " + name, true, "Unknown tool " + name};
    return tool->run(state, args);
}

// Runs the waiting tool calls for the innermost screen unless a command runs, then lets the screen show their effect. UI thread only.
// Posted by the server whenever tool calls wait, and called by the screens when they are shown and after commands.
export void RunWaiting() {
    if (!state.server || state.screens.empty() || InteractiveCmds::CommandRunning()) return;
    Screen screen = state.screens.back(); // Copy, after may show further screens (e.g. open a report)
    std::vector<Activity> done;
    state.server->ProcessPending(Run, [&](Activity const& entry) {
        state.log.push_back(entry);
        done.push_back(entry);
    });
    screen.after(done);
}

export bool Running() { return state.server != nullptr; }
// Whether MCP mode is active, i.e. tool calls are run. It is left by the TUI only.
export bool Active() { return state.server && state.active; }
export std::string Url() { return state.server ? state.server->Url() : ""; }
export std::string AuthHeader() { return state.server ? state.server->AuthHeader() : ""; }
export std::vector<Activity> const& Log() { return state.log; }

// Starts the server unless running and enters MCP mode, returns an error message on failure.
// post is called from the server thread.
export std::string Start(PostFn post) {
    if (!state.server) {
        MCPServer::StartResult started = MCPServer::Start(TraceTools::SpecsOf(Tools()), [post] { post(RunWaiting); });
        if (!started.server) return started.message;
        state.server = std::move(started.server);
    }
    state.active = true;
    state.server->SetAccepting(true); // Tool calls wait while no screen is ready, e.g. during the analysis
    return "";
}

// Leaves MCP mode, tool calls then fail with the refusal message until it is started again
export void Leave(std::string const& refusal) {
    state.active = false;
    if (state.server) state.server->SetAccepting(false, refusal);
}

export Action PendingAction() { return state.pending; }

// Opens the trace requested by a tool call (OPEN_REPORT), returns whether to reanalyse. While the results are shown, see ResultsScope.
export bool OpenPendingReport() {
    Action action = state.pending;
    state.pending = {};
    ReportedFormula const& report = (*state.results)[action.report];
    std::shared_ptr<ContractTree::ContractExpression> leaf = DebuggableLeaves(report, *state.result_debug_handlers)[action.leaf];
    return (*state.result_debug_handlers)[leaf.get()](ContractManagerAnalysis::DebugContext(*report.contract, *report.formula, *leaf, *state.result_texts));
}

// What a trace screen does after tool calls ran, see NavigateTrace
export enum struct TraceNavigation { STAY, REANALYSED, CLOSE };

// Performs the navigation requested by tool calls while a trace is open. A reanalysis is run with run_command, so it is
// recorded in the history like the user's commands. CLOSE: the screen closes the trace, the results screen opens the requested report.
export TraceNavigation NavigateTrace(std::function<void(std::string const&)> const& run_command) {
    switch (state.pending.kind) {
        case Action::Kind::NONE: return TraceNavigation::STAY;
        case Action::Kind::OPEN_REPORT: return TraceNavigation::CLOSE;
        case Action::Kind::REANALYSE:
            state.pending = {};
            run_command("reanalyse");
            return TraceNavigation::REANALYSED;
    }
    return TraceNavigation::STAY;
}

// The results of the current analysis are shown. Requests to open the trace debugged before the reanalysis
// again if it is still reported, see CheckResume.
export class ResultsScope {
    public:
        ResultsScope(std::vector<ReportedFormula> const& formulas, ContractManagerAnalysis::FormulaReports const& reports, ContractManagerAnalysis::FormulaDebugHandlers& debugHandlers)
            : prev{state.results}, prev_texts{state.result_texts}, prev_debug_handlers{state.result_debug_handlers} {
            state.results = &formulas;
            state.result_texts = &reports;
            state.result_debug_handlers = &debugHandlers;
            if (!state.resume) return;
            for (int report = 0; report < formulas.size(); report++) {
                std::vector<std::shared_ptr<ContractTree::ContractExpression>> leaves = DebuggableLeaves(formulas[report], debugHandlers);
                for (int leaf = 0; leaf < leaves.size(); leaf++) {
                    if (TraceKey(formulas[report], *leaves[leaf]) == *state.resume) state.pending = {Action::Kind::OPEN_REPORT, report, leaf};
                }
            }
            state.resume.reset();
        }
        ~ResultsScope() {
            state.results = prev;
            state.result_texts = prev_texts;
            state.result_debug_handlers = prev_debug_handlers;
        }
    private:
        std::vector<ReportedFormula> const* prev;
        ContractManagerAnalysis::FormulaReports const* prev_texts;
        ContractManagerAnalysis::FormulaDebugHandlers* prev_debug_handlers;
};

// A trace is open in the debugger
export class TraceScope {
    public:
        TraceScope(TraceTools::TraceTools& tools) : prev{state.trace} { state.trace = &tools; }
        ~TraceScope() { state.trace = prev; }
    private:
        TraceTools::TraceTools* prev;
};

// A screen of the frontend that runs tool calls while it is shown, so it must not outlive the screen. Tool calls run for the
// innermost screen while no command runs (see InteractiveCmds::CommandRunning). Afterwards after is called with the calls that ran
// (possibly none), e.g. to show their effect and perform the requested navigation.
export class ScreenScope {
    public:
        explicit ScreenScope(std::function<void(std::vector<Activity> const&)> after) {
            state.screens.push_back({std::move(after)});
        }
        ~ScreenScope() { state.screens.pop_back(); }
        ScreenScope(ScreenScope const&) = delete;
        ScreenScope& operator=(ScreenScope const&) = delete;
};

// When reanalysing from the trace of context: if MCP mode is active, it continues after the reanalysis with its report.
// Appended to the history cache, so call it after TraceSession::SaveHistory.
export void SaveResume(ContractManagerAnalysis::DebugContext const& context) {
    if (!Active()) return;
    std::ofstream cache(InteractiveUtils::HISTORY_CACHE_FILE, std::ios::app);
    cache << RESUME_PREFIX << TraceKey(context) << "\n";
}

// At the start of a reanalysis: whether MCP mode was active before, then the frontend starts it again.
// The history cache is left for the trace session.
export bool CheckResume() {
    std::ifstream cache(InteractiveUtils::HISTORY_CACHE_FILE);
    for (std::string line; std::getline(cache, line);) {
        if (!line.starts_with(RESUME_PREFIX)) continue;
        state.resume = line.substr(RESUME_PREFIX.size());
        return true;
    }
    return false;
}

} // namespace MCPControl
