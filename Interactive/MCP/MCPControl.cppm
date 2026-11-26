module;

#include <json/json.h>

#include "ContractTree.hpp"

export module MCPControl;
import std;
import AnalysisReports;
import ErrorMessage;
import InteractiveUtils;
import InteractiveSession;
import InteractiveCmds;
import TraceTools;
import MCPServer;
import LLVMModule;

using namespace ContractTree;

using namespace llvm;

// MCP for the whole interactive session: owns the server, and dispatches tool calls depending on what is shown.
// Besides the trace tools, MCP clients can list the reports, open their traces and reanalyse, so they can work through all reports.
// The frontends declare what is shown with the scopes below and perform the requested navigation (OpenPendingReport, NavigateTrace).
namespace MCPControl {

using InteractiveUtils::DebuggableLeaves;
using InteractiveUtils::PostFn;
using InteractiveUtils::ReportedFormula;
using InteractiveUtils::TraceKey;
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
- A report only shows the conflicts of the first call that violates the contract, and on each path only the first conflict after that call. The other calls and conflicts are checked once these are fixed. Annotate the matching values of all calls (e.g. the stores of all fill loops) before reanalysing.
- reanalyse reruns the analysis with all annotations added so far and returns the new reports. If the leaf whose trace was open is still reported afterwards, its trace is opened again automatically.
- After reanalysing, check with list_reports that the reports you annotated are gone. Annotations can also uncover further reports.
- Annotate false positives only. Leave real bugs reported, and tell the user about them with a suggested fix.
- When done, tell the user which annotations you added for which reports, and which reports are real bugs. The user can save the annotations as a patch file with the diff command of CoVer's debugger, or from the report list.)";

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
    AnalysisReports::FormulaReports const* result_texts = nullptr;
    AnalysisReports::FormulaDebugHandlers* result_debug_handlers = nullptr;
    TraceTools::ToolTarget target; // What the trace tools work on: the module while the results are shown, and the open trace
    std::vector<Screen> screens; // Innermost last
    std::string start_error; // Why the server could not be started
};

State state;

std::string OpenTraceKey(State const& s) {
    return s.target.trace ? TraceKey(s.target.trace->Context()) : "";
}

std::string ReportsText(State const& s, bool details) {
    if (!s.results) return "# Reports\nNot available yet, the analysis is still running.\n";
    std::string open_key = OpenTraceKey(s);
    std::string out = std::format("# Reports\n{} reports.{}\n", s.results->size(), details ? "" : " list_reports shows their formulas, error reports and the leaves whose traces can be opened.");
    for (int i = 0; i < s.results->size(); i++) {
        ReportedFormula const& report = (*s.results)[i];
        std::vector<std::shared_ptr<ContractTree::ContractExpression>> leaves = DebuggableLeaves(report, *s.result_debug_handlers);
        if (!details) {
            bool open = std::ranges::any_of(leaves, [&](std::shared_ptr<ContractTree::ContractExpression> const& leaf) { return !open_key.empty() && TraceKey(report, *leaf) == open_key; });
            out += std::format("Report {}: {}{}\n", i, report.Summary(), leaves.empty() ? " [no debuggable trace]" : open ? " [trace open]" : "");
        } else {
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
    }
    // Only once nothing is reported anymore, so that it does not distract from the reports
    return s.results->empty() ? out + TraceTools::EpochReminder() : out;
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

// Responds once the reanalysis is done, see AnswerReanalysis
ToolResult Reanalyse(State& s, Json::Value const&) {
    if (!s.results) return {"The analysis is still running", true, "reanalyse  ->  analysis running"};
    s.pending = {Action::Kind::REANALYSE};
    s.server->SetAccepting(false, MCPServer::RESTARTING); // Calls until this process exits would get lost
    s.server->DeferResponse();
    return {"", false, "reanalyse"}; // Not sent
}

// Answers the reanalyse call of the previous run of this process, if it was deferred (see Reanalyse). Once the results or a trace are shown,
// as that is when the tool calls of this run are executed.
void AnswerReanalysis(State const& s) {
    std::optional<std::string> saved = InteractiveSession::TakeMCPResponse();
    if (!saved || !s.results) return;
    std::optional<MCPServer::Deferred> deferred = MCPServer::ParseDeferred(*saved);
    if (!deferred) return;
    MCPServer::SendDeferred(*deferred, {"Reanalysis done, with all annotations added so far.\n" + ReportsText(s, true), false, "reanalyse"});
}

// The trace tools, available depending on what is shown (see TraceTools::ToolNeeds), followed by the navigation tools
std::vector<TraceTools::Tool<State>> BuildTools() {
    std::vector<TraceTools::Tool<State>> tools;
    for (ToolSpec const& spec : TraceTools::Specs()) {
        if (spec.name == TraceTools::CONTEXT_TOOL) { // Extended by the workflow and the reports
            tools.push_back({spec, [](State& s, Json::Value const&) -> ToolResult {
                return {TraceTools::GUIDANCE + "\n\n" + WORKFLOW + "\n\n" + TraceTools::Context(s.target) + "\n" + ReportsText(s, false), false, TraceTools::CONTEXT_TOOL};
            }});
            continue;
        }
        tools.push_back({spec, [name = spec.name](State& s, Json::Value const& args) { return TraceTools::Run(name, args, s.target); }});
    }
    Json::Value open_schema = TraceTools::Schema({{"report", "integer", "Report number, see list_reports"}, {"leaf", "integer", "Index of the violated leaf whose trace to open, see list_reports"}}, {"report", "leaf"});
    tools.push_back({{"list_reports", "Reports of the current analysis with their number, contract, message, error report and locations, and the violated leaves of their formula that have a trace, with their index and whose trace is open.", TraceTools::Schema({}, {})},
                     [](State& s, Json::Value const&) -> ToolResult { return {ReportsText(s, true), false, "list_reports"}; }});
    tools.push_back({{"open_report", "Open the trace of a violated leaf of a report in the trace debugger, closing the currently open trace. Annotations added so far are kept.", open_schema}, OpenReport});
    tools.push_back({{"reanalyse", "Rerun the analysis with all annotations added so far, returns the new reports once it is done.", TraceTools::Schema({}, {})}, Reanalyse});
    return tools;
}

std::vector<TraceTools::Tool<State>> const& Tools() {
    static std::vector<TraceTools::Tool<State>> const tools = BuildTools();
    return tools;
}

// Tool calls arriving while navigation is pending wait until it is done, see RunWaiting
ToolResult Run(std::string const& name, Json::Value const& args) {
    TraceTools::Tool<State> const* tool = TraceTools::FindTool(Tools(), name);
    if (!tool) return {"Unknown tool " + name, true, "Unknown tool " + name};
    return tool->run(state, args);
}

// Runs the waiting tool calls for the innermost screen unless a command runs, then lets the screen show their effect. UI thread only.
// Posted by the server whenever tool calls wait, and called by the screens when they are shown and after commands.
// Once a tool call requests navigation, the calls after it wait until it is done: those following open_report run on the opened trace,
// those following reanalyse fail, as this process restarts for the reanalysis (see Reanalyse).
export void RunWaiting() {
    if (!state.server || state.screens.empty() || InteractiveCmds::CommandRunning()) return;
    Screen screen = state.screens.back(); // Copy, after may show further screens (e.g. open a report)
    std::vector<Activity> done;
    state.server->ProcessPending(Run, [&](Activity const& entry) {
        state.log.push_back(entry);
        done.push_back(entry);
    }, [] { return state.pending.kind != Action::Kind::NONE; });
    screen.after(done);
}

export bool Running() { return state.server != nullptr; }
export std::string const& StartError() { return state.start_error; }
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
        if (!started.server) return state.start_error = started.message;
        state.server = std::move(started.server);
        state.start_error.clear();
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

// The response to the reanalyse call, which the next run of this process sends once the reanalysis is done (see AnswerReanalysis).
// For the history cache, see InteractiveSession::SaveForReanalysis.
export std::optional<std::string> DeferredReanalysis() {
    if (!state.server || !state.server->DeferredResponse()) return std::nullopt;
    return MCPServer::Serialize(*state.server->DeferredResponse());
}

// Opens the trace requested by a tool call (OPEN_REPORT), returns whether to reanalyse. While the results are shown, see ResultsScope.
export bool OpenPendingReport() {
    Action action = state.pending;
    state.pending = {};
    ReportedFormula const& report = (*state.results)[action.report];
    std::shared_ptr<ContractTree::ContractExpression> leaf = DebuggableLeaves(report, *state.result_debug_handlers)[action.leaf];
    return (*state.result_debug_handlers)[leaf.get()](AnalysisReports::DebugContext(*report.contract, *report.formula, *leaf, *state.result_texts));
}

// Takes a reanalysis requested by a tool call while no trace is open, returns whether there was one. The frontends' results screens
// return to reanalyse once one is pending. It is recorded in the history like a reanalysis from a trace (see NavigateTrace), which also
// writes the module to reanalyse. While the results are shown, see ResultsScope.
export bool TakeResultsReanalysis() {
    if (state.pending.kind != Action::Kind::REANALYSE) return false;
    state.pending = {};
    InteractiveCmds::ExecuteModuleCommand(*state.target.module, "reanalyse", {});
    return true;
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

// The results of the current analysis of M are shown. The trace tools work on M until a trace is opened.
export class ResultsScope {
    public:
        ResultsScope(Module& M, std::vector<ReportedFormula> const& formulas, AnalysisReports::FormulaReports const& reports, AnalysisReports::FormulaDebugHandlers& debugHandlers)
            : prev{state.results}, prev_texts{state.result_texts}, prev_debug_handlers{state.result_debug_handlers}, prev_target{state.target} {
            state.results = &formulas;
            state.result_texts = &reports;
            state.result_debug_handlers = &debugHandlers;
            state.target = {&M};
        }
        ~ResultsScope() {
            state.results = prev;
            state.result_texts = prev_texts;
            state.result_debug_handlers = prev_debug_handlers;
            state.target = prev_target;
        }
    private:
        std::vector<ReportedFormula> const* prev;
        AnalysisReports::FormulaReports const* prev_texts;
        AnalysisReports::FormulaDebugHandlers* prev_debug_handlers;
        TraceTools::ToolTarget prev_target;
};

// A trace is open in the debugger
export class TraceScope {
    public:
        TraceScope(InteractiveSession::TraceSession& trace) : prev{state.target} { state.target = TraceTools::TargetOf(trace); }
        ~TraceScope() { state.target = prev; }
    private:
        TraceTools::ToolTarget prev;
};

// A screen of the frontend that runs tool calls while it is shown, so it must not outlive the screen. Tool calls run for the
// innermost screen while no command runs (see InteractiveCmds::CommandRunning). Afterwards after is called with the calls that ran
// (possibly none), e.g. to show their effect and perform the requested navigation.
export class ScreenScope {
    public:
        explicit ScreenScope(std::function<void(std::vector<Activity> const&)> after) {
            state.screens.push_back({std::move(after)});
            AnswerReanalysis(state);
        }
        ~ScreenScope() { state.screens.pop_back(); }
        ScreenScope(ScreenScope const&) = delete;
        ScreenScope& operator=(ScreenScope const&) = delete;
};

} // namespace MCPControl
