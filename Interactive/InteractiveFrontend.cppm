export module InteractiveFrontend;
import std;
import AnalysisReports;
import InteractiveSession;
import InteractiveUtils;

// Interface of the frontend libraries of the interactive mode, loaded on demand.
namespace InteractiveFrontend {

// Name of the function each frontend library exports with C linkage. It returns the library's Frontend, which lives as long as the program.
export constexpr char const* ENTRY_POINT = "CoVer_Frontend";

export struct Frontend {
    // Whether the frontend is currently available
    bool (*isAvailable)();
    // Starts the MCP server and enters MCP mode, used to continue MCP mode after a reanalysis. Returns an error message on failure.
    std::string (*startMCP)();
    // The contracts before the analysis
    void (*startMenu)(std::vector<AnalysisReports::Contract> const& contracts, AnalysisReports::FormulaReports const& reports,
                      AnalysisReports::FormulaDebugHandlers& debugHandlers, double parse_time);
    // The reports after the analysis, returns whether to reanalyse
    bool (*resultsScreen)(std::vector<InteractiveUtils::ReportedFormula> const& formulas, AnalysisReports::FormulaReports const& reports,
                          AnalysisReports::FormulaDebugHandlers& debugHandlers, double runtime);
    // The trace debugger, returns whether to reanalyse
    bool (*showTrace)(InteractiveSession::TraceSession& session);
};

export using EntryPoint = Frontend const* (*)();

} // namespace InteractiveFrontend
