module;

#include "ContractTree.hpp"

export module InteractiveUtils;
import std;
import AnalysisReports;
import LLVMModule;
import ErrorMessage;
import ContractPassUtility;
import Worklist;

using namespace ContractTree;

using namespace llvm;

// Frontend independent parts of the interactive mode, shared by the TUI and GUI. Trace commands are in InteractiveCmds.
namespace InteractiveUtils {

using Contract = AnalysisReports::Contract;

export constexpr int UI_ANALYSISINFO_PAD_SIZE = 15;

// Runs a task on the frontend's UI thread, callable from any thread (TUIManager::Post, GUIManager::Post).
// Worker threads may call it after the object they were started by is gone, so it must stay valid for the program's lifetime.
export using PostFn = std::function<void(std::function<void()>)>;

// Files of the interactive mode, in TMP_DIR in the working directory. The compile wrappers refer to REANALYSE_FILE by name.
export constexpr char const* TMP_DIR = "CoVerTmp";
export constexpr char const* INTERACT_START_FILE = "CoVerTmp/InteractStart.ll"; // Module before any annotation, base of the diff command
export constexpr char const* REANALYSE_FILE = "CoVerTmp/Reanalyse.ll"; // Current module, input of the reanalysis
export constexpr char const* HISTORY_CACHE_FILE = "CoVerTmp/HistoryCache"; // Command history and MCP mode, carried over to the reanalysis
export constexpr char const* CHAT_CACHE_FILE = "CoVerTmp/ChatCache.json"; // Conversation of the LLM chat, carried over to the reanalysis
export constexpr char const* DIFF_TMP_FILE = "CoVerTmp/Diff.tmp";

export void removeInteractiveFiles() {
    for (char const* file : {INTERACT_START_FILE, REANALYSE_FILE, HISTORY_CACHE_FILE, CHAT_CACHE_FILE, DIFF_TMP_FILE}) std::filesystem::remove(file);
    std::filesystem::remove(TMP_DIR);
}

std::string getSpecificLine(std::string file, int line) {
    std::ifstream filestream(file);
    std::string out;
    for (int i = 0; i < line; i++) {
        std::getline(filestream, out);
    }
    return out;
}

// Duration in seconds, as milliseconds below one second
export std::string formatDuration(double seconds) {
    return seconds < 1 ? std::format("{:.1f}ms", seconds * 1000) : std::format("{:.2f}s", seconds);
}

export std::string startMenuTitle(std::size_t num_contracts, double parse_time) {
    return std::format("Start Menu ({} contracts to be analysed, parsed in {})", num_contracts, formatDuration(parse_time));
}

export std::string resultsTitle(bool has_reports, double runtime) {
    return std::format("{} (analysed in {})", has_reports ? "Reported Errors" : "No reported errors!", formatDuration(runtime));
}

export struct ReportedFormula {
    std::shared_ptr<ContractTree::ContractFormula> formula;
    Contract const* contract;

    std::string Summary() const { return std::format("{}: {}", contract->F->getName().str(), formula->Message.value_or(formula->ExprStr)); }
};

// Identifies a trace across analysis runs: one is reported per contract and top level formula, and has a trace per violated leaf.
// Leaf indices may change when a reanalysis fixes other leaves, so the leaf's formula is used.
export std::string TraceKey(std::string const& function, std::string const& report, std::string const& leaf) {
    return function + "|" + report + "|" + leaf;
}

export std::string TraceKey(ReportedFormula const& report, ContractTree::ContractFormula const& leaf) {
    return TraceKey(report.contract->F->getName().str(), report.formula->ExprStr, leaf.ExprStr);
}

export std::string TraceKey(AnalysisReports::DebugContext const& context) {
    return TraceKey(context.function, context.report, context.formula);
}

// Formulas listed on the results screen, all that are not fulfilled
export std::vector<ReportedFormula> getReportedFormulas(std::vector<Contract> const& ViolatedContracts) {
    std::vector<ReportedFormula> formulas;
    for (Contract const& C : ViolatedContracts) {
        for (std::shared_ptr<ContractTree::ContractFormula> const& scope : {C.Data.Pre, C.Data.Post}) {
            if (!scope) continue;
            for (std::shared_ptr<ContractTree::ContractFormula> const& form : scope->Children) {
                if (*form->Status != ContractTree::Fulfillment::FULFILLED) formulas.push_back({form, &C});
            }
        }
    }
    return formulas;
}

// Violated leaves of the formula with a trace that can be debugged, in depth-first order. MCP clients address them by their index.
void CollectLeaves(std::shared_ptr<ContractTree::ContractFormula> const& form, AnalysisReports::FormulaDebugHandlers const& debugHandlers,
                   std::vector<std::shared_ptr<ContractTree::ContractExpression>>& leaves) {
    if (*form->Status != ContractTree::Fulfillment::FULFILLED && debugHandlers.contains(form.get())) leaves.push_back(std::static_pointer_cast<ContractTree::ContractExpression>(form));
    for (std::shared_ptr<ContractTree::ContractFormula> const& child : form->Children) CollectLeaves(child, debugHandlers, leaves);
}

export std::vector<std::shared_ptr<ContractTree::ContractExpression>> DebuggableLeaves(ReportedFormula const& report, AnalysisReports::FormulaDebugHandlers const& debugHandlers) {
    std::vector<std::shared_ptr<ContractTree::ContractExpression>> leaves;
    CollectLeaves(report.formula, debugHandlers, leaves);
    return leaves;
}

export std::string traceKindToStr(Worklist::TraceKind kind) {
    switch (kind) {
        case Worklist::TraceKind::LINEAR: return "LINEAR";
        case Worklist::TraceKind::BRANCH: return "BRANCH";
        case Worklist::TraceKind::SWITCH: return "SWITCH";
        case Worklist::TraceKind::FUNCENTRY: return "FUNCENTRY";
        case Worklist::TraceKind::FUNCEXIT: return "FUNCEXIT";
    }
}

// Attributes that only inform LLVM's optimisations, left out of the IR shown to language models and in alias-get. Attributes changing how a
// value is passed (byval, sret, ...) and noalias, which concerns aliasing, are kept.
constexpr std::array HINT_ATTRIBUTES = std::to_array<std::string_view>({
    "align", "captures", "dead_on_return", "dead_on_unwind", "dereferenceable", "dereferenceable_or_null", "immarg", "initializes", "inreg",
    "nocapture", "nofpclass", "nofree", "noext", "nonnull", "noundef", "range", "readnone", "readonly", "returned", "signext", "writable",
    "writeonly", "zeroext"});

// Keywords that only inform LLVM's optimisations and code generation, left out as well: poison generating flags of instructions
// (nsw, inbounds, ...), fast-math flags, tail call markers, and linkage hints of globals and functions
constexpr std::array HINT_KEYWORDS = std::to_array<std::string_view>({
    "nsw", "nuw", "nusw", "inbounds", "exact", "disjoint", "nneg", "samesign",
    "nnan", "ninf", "nsz", "arcp", "contract", "afn", "reassoc", "fast",
    "tail", "notail", "dso_local", "dso_preemptable", "unnamed_addr", "local_unnamed_addr"});

// A line of printed IR without metadata attachments (", !dbg !12"), attribute group references (#0), alignment
// (", align 4"), the HINT_ATTRIBUTES of parameters and return values and the HINT_KEYWORDS, which only matter to LLVM.
// Runs of spaces after the indentation (e.g. before the predecessors of a block) are shortened to one.
export std::string CompactIR(std::string_view ir) {
    // Metadata attachments of instructions come last
    for (std::size_t attach = ir.find(", !"); attach != std::string_view::npos; attach = ir.find(", !", attach + 1)) {
        if (attach + 3 < ir.size() && std::isalpha(static_cast<unsigned char>(ir[attach + 3]))) {
            ir = ir.substr(0, attach);
            break;
        }
    }
    auto is_word_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; };
    std::string out;
    std::size_t pos = 0;
    while (pos < ir.size()) {
        char c = ir[pos];
        if (c == '"') { // String constants and quoted names are kept as they are
            std::size_t end = ir.find('"', pos + 1);
            end = end == std::string_view::npos ? ir.size() : end + 1;
            out += ir.substr(pos, end - pos);
            pos = end;
            continue;
        }
        bool token_start = pos == 0 || ir[pos - 1] == ' ' || ir[pos - 1] == '(';
        // A word, or a name with its prefix (%x, @f, #0, !dbg) so that names are never mistaken for attributes
        std::size_t end = pos + 1;
        if (is_word_char(c) || c == '%' || c == '@' || c == '#' || c == '!') {
            while (end < ir.size() && is_word_char(ir[end])) end++;
        }
        std::string_view token = ir.substr(pos, end - pos);
        bool drop = false;
        if (c == ' ' && out.ends_with(' ') && out.find_first_not_of(' ') != std::string::npos) { // Repeated space after the indentation
            pos = end;
            continue;
        }
        if (token_start && std::ranges::contains(HINT_KEYWORDS, token)) {
            drop = true;
        } else if (token_start && c == '#' && token.size() > 1) { // Attribute group, e.g. "#3"
            drop = std::ranges::all_of(token.substr(1), [](char d) { return std::isdigit(static_cast<unsigned char>(d)); });
        } else if (token_start && c == '!' && token.size() > 1 && ir.substr(end).starts_with(" !")) { // Metadata attachment of a function, e.g. "!dbg !12"
            drop = true;
            end += 2;
            while (end < ir.size() && is_word_char(ir[end])) end++;
        } else if (token_start && std::ranges::contains(HINT_ATTRIBUTES, token)) {
            drop = true;
            if (end < ir.size() && ir[end] == '(') { // Argument in parentheses, e.g. "dereferenceable(8)"
                for (int depth = 0; end < ir.size(); end++) {
                    if (ir[end] == '(') depth++;
                    else if (ir[end] == ')' && --depth == 0) break;
                }
                end = std::min(end + 1, ir.size());
            } else if (token == "align" && ir.substr(end).starts_with(" ")) { // Alignment, e.g. "align 8"
                end++;
                while (end < ir.size() && std::isdigit(static_cast<unsigned char>(ir[end]))) end++;
            }
        }
        if (!drop) {
            out += token;
            pos = end;
            continue;
        }
        // Remove one space next to the dropped token, and a comma left without an operand (", align 4" at the end)
        if (end < ir.size() && ir[end] == ' ') end++;
        else if (out.ends_with(' ')) out.pop_back();
        if (out.ends_with(',') && (end == ir.size() || ir[end] == ',')) out.pop_back();
        pos = end;
    }
    return out;
}

// Source code of the instruction's line, empty without debug info
export std::string getSourceCode(Instruction const* I) {
    if (!I->getDebugLoc()) return "";
    FileReference ref = ContractPassUtility::getFileReference(I);
    return getSpecificLine(ref.file, ref.line);
}

export std::string getSourceLine(Instruction const* I, bool translate) {
    if (translate) {
        if (!I->getDebugLoc()) return "";
        FileReference ref = ContractPassUtility::getFileReference(I);
        return std::format("{:>15}:{:0>3} | {}", std::filesystem::path(ref.file).filename().string(), ref.line, getSourceCode(I));
    } else {
        std::string out;
        raw_string_ostream strstream(out);
        I->print(strstream);
        return out;
    }
}

} // namespace InteractiveUtils
