module;

#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

export module InteractiveSession;
import LLVMModule;
import ContractPassUtility;
import InteractiveUtils;

using namespace llvm;

// Frontend independent state of the trace debugger. The frontends and trace commands only see the non-template TraceSession.
namespace InteractiveSession {

using ContractPassUtility::JumpTraceEntry;
using ContractPassUtility::TraceDB;
using ContractPassUtility::TraceKind;

export enum struct CmdResultCode {
    UNKNOWN_COMMAND,
    INVALID_SYNTAX,
    INVALID_INPUT,
    SUCCESS_CONTINUE,
    SUCCESS_REANALYSE,
    SUCCESS_EXIT
};
export struct CmdResult {
    CmdResultCode code;
    std::string res;
};
export using History = std::vector<std::pair<std::string, CmdResult>>;

export std::string resultCodeStr(CmdResultCode code) {
    switch (code) {
        case CmdResultCode::UNKNOWN_COMMAND: return "Unknown Command";
        case CmdResultCode::INVALID_SYNTAX: return "Invalid Syntax";
        case CmdResultCode::INVALID_INPUT: return "Invalid Input";
        default: return "Success";
    }
}

constexpr std::string_view HISTORY_BEGIN = "|| DIFF HISTORY BEGIN ||";
constexpr std::string_view HISTORY_END = "|| DIFF HISTORY END ||";

// Command history of a previous run, if this is a reanalysis
History LoadHistoryCache() {
    History input_history;
    if (std::filesystem::exists(InteractiveUtils::HISTORY_CACHE_FILE)) {
        std::string line;
        std::ifstream historyfile(InteractiveUtils::HISTORY_CACHE_FILE);
        std::getline(historyfile, line);
        while (std::getline(historyfile, line)) {
            if (line == HISTORY_END) break;
            std::string_view code_str = std::string_view(line).substr(0, line.find('|'));
            std::underlying_type_t<CmdResultCode> code = 0;
            std::from_chars(code_str.data(), code_str.data() + code_str.size(), code);
            input_history.push_back({line.substr(line.find('|') + 1), {static_cast<CmdResultCode>(code), ""}});
        }
        std::filesystem::remove(InteractiveUtils::HISTORY_CACHE_FILE);
    }
    return input_history;
}

export struct TraceEntryView {
    Instruction* loc; // Also identifies the entry across refreshes, as there is one per instruction
    std::string info;
};

export struct TraceBlockView {
    int index;
    TraceEntryView first;
    std::string description;
    std::string start_loc;
    std::string end_loc;
    std::optional<int> jump_to; // Set if the block is a repetition of an earlier block (loop or goto)
    bool expanded;
    TraceKind kind; // How the last entry is reached from its children
    std::vector<TraceEntryView> children; // Predecessors of the last entry, empty for blocks repeating an earlier one
    int selected_child = 0;
};

export struct BlockLine {
    int instr; // Index of the instruction, or index of first if aggregate due to source approx
    std::string text;
};

// Views are rebuilt by Refresh, which the trace commands call after every command except for those ending the session
export class TraceSession {
    public:
        virtual ~TraceSession() = default;
        virtual void Refresh() = 0;
        virtual std::vector<TraceBlockView> const& Blocks() const = 0;
        // Entries of a block, numbered like the instruction arguments of the trace commands
        virtual std::vector<TraceEntryView> BlockEntries(int block) const = 0;
        virtual int NumChildren(int block) const = 0;
        virtual void SelectChild(int block, int child) = 0;
        virtual void SetExpanded(int block, bool expanded) = 0;

        // Lines of a block, skipping instructions without source line and repetitions of the previous line
        std::vector<BlockLine> BlockLines(int block, bool source) const {
            std::vector<BlockLine> lines;
            std::string last;
            for (auto const [instr, entry] : std::views::enumerate(BlockEntries(block))) {
                std::string line = InteractiveUtils::getSourceLine(entry.loc, source);
                if (line.empty() || line == last) continue;
                lines.emplace_back(instr, std::format("{:>{}} | {}", entry.info, InteractiveUtils::UI_ANALYSISINFO_PAD_SIZE, line));
                last = std::move(line);
            }
            return lines;
        }

        // nullptr if there is no such instruction
        Instruction* BlockInstruction(int block, int instr) const {
            std::vector<TraceEntryView> entries = BlockEntries(block);
            return instr >= 0 && instr < entries.size() ? entries[instr].loc : nullptr;
        }

        Function* BlockFunction(int block) const { return Blocks()[block].first.loc->getFunction(); }
        Module* GetModule() const { return Blocks().front().first.loc->getModule(); }

        // Show instructions of expanded blocks as source code approximation, see toggle-view
        bool ExpandAsSource() const { return expand_as_source; }
        void SetExpandAsSource(bool source) { expand_as_source = source; }

        History const& GetHistory() const { return history; }
        void AddHistory(std::string const& cmd, CmdResult const& res) { history.push_back({cmd, res}); }
        void SaveHistory() const {
            std::ofstream cache(InteractiveUtils::HISTORY_CACHE_FILE);
            WriteHistory(cache, true);
        }

        // History framed as in the patch files of the diff command, with result codes for the history cache
        void WriteHistory(std::ostream& out, bool with_codes) const {
            out << HISTORY_BEGIN << "\n";
            for (auto const& [command, result] : history) {
                if (with_codes) out << std::to_underlying(result.code) << "|";
                out << command << "\n";
            }
            out << HISTORY_END << "\n---\n";
        }

    protected:
        bool expand_as_source = false;
        History history = LoadHistoryCache();
};

// Current module, used as input for reanalysis and diffing
void WriteReanalyseModule(Module const* M) {
    std::error_code rc;
    raw_fd_stream reanalyse_file(InteractiveUtils::REANALYSE_FILE, rc);
    M->print(reanalyse_file, nullptr);
}

// Follows single predecessors up to the end of a linear part of the trace, always stopping at function entries and exits
template<typename T>
JumpTraceEntry<T>* getLinearTraceEnd(JumpTraceEntry<T>* entry) {
    while (entry->predecessors.size() == 1) {
        entry = entry->predecessors[0];
        if (entry->kind == TraceKind::FUNCEXIT || entry->kind == TraceKind::FUNCENTRY) break;
    }
    return entry;
}

export template<typename T>
class TraceSessionImpl : public TraceSession {
    public:
        TraceSessionImpl(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr) : traceDB{traceDB}, trace{trace}, infoToStr{infoToStr} {
            Refresh();
        }

        // Splits the trace into linear blocks, following the selected children until the trace ends or repeats a block
        void Refresh() override {
            WriteReanalyseModule(trace->loc->getModule());
            blocks.clear();
            ranges.clear();
            std::map<JumpTraceEntry<T>*, int> block_starts;
            JumpTraceEntry<T>* first = trace;
            while (first) {
                JumpTraceEntry<T>* last = getLinearTraceEnd(first);
                TraceBlockView view = {
                    .index = static_cast<int>(blocks.size()),
                    .first = EntryView(first),
                    .start_loc = StartLoc(first),
                    .end_loc = EndLoc(last),
                    .expanded = expanded.contains(last),
                    .kind = last->kind,
                };
                JumpTraceEntry<T>* next = nullptr;
                if (block_starts.contains(first)) {
                    view.jump_to = block_starts[first];
                    view.description = std::format("From {} to {} then jump to block {}", view.start_loc, view.end_loc, *view.jump_to);
                } else {
                    block_starts[first] = view.index;
                    view.description = std::format("From {} to {}", view.start_loc, view.end_loc);
                    view.selected_child = sibling_select.contains(last) ? sibling_select[last] : 0;
                    for (JumpTraceEntry<T>* child : last->predecessors) view.children.push_back(EntryView(child));
                    if (!view.children.empty()) {
                        view.description += std::format(" then {}", InteractiveUtils::traceKindToStr(last->kind));
                        if (view.children.size() > 1) view.description += std::format(" [Viewing Child {}/{}]", view.selected_child, view.children.size() - 1);
                        next = last->predecessors[view.selected_child];
                    }
                }
                blocks.push_back(view);
                ranges.push_back({first, last});
                first = next;
            }
        }

        std::vector<TraceBlockView> const& Blocks() const override { return blocks; }

        std::vector<TraceEntryView> BlockEntries(int block) const override {
            std::vector<TraceEntryView> entries = {EntryView(ranges[block].first)};
            for (JumpTraceEntry<T>* entry = ranges[block].first; entry != ranges[block].last;) {
                entry = entry->predecessors[0];
                entries.push_back(EntryView(entry));
            }
            return entries;
        }

        int NumChildren(int block) const override { return ranges[block].last->predecessors.size(); }

        void SelectChild(int block, int child) override { sibling_select[ranges[block].last] = child; }

        void SetExpanded(int block, bool expand) override {
            if (expand) expanded.insert(ranges[block].last);
            else expanded.erase(ranges[block].last);
        }

    private:
        struct BlockRange {
            JumpTraceEntry<T>* first;
            JumpTraceEntry<T>* last;
        };

        TraceDB<T> traceDB; // Owns the entries
        JumpTraceEntry<T>* trace;
        std::function<std::string(T)> infoToStr;
        std::map<JumpTraceEntry<T>*,int> sibling_select;
        std::set<JumpTraceEntry<T>*> expanded;
        std::vector<BlockRange> ranges;
        std::vector<TraceBlockView> blocks;

        TraceEntryView EntryView(JumpTraceEntry<T> const* entry) const { return {entry->loc, infoToStr(entry->analysisInfo)}; }

        static std::string StartLoc(JumpTraceEntry<T> const* first) {
            std::string loc = ContractPassUtility::getInstrLocStr(first->loc, false);
            if (loc == "UNKNOWN" && first->loc->getPrevNode()) loc = ContractPassUtility::getInstrLocStr(first->loc->getPrevNode(), false); // Second try
            return loc;
        }

        static std::string EndLoc(JumpTraceEntry<T> const* last) {
            Function const* F = last->loc->getFunction();
            // Function begin does not have dbg info, but header does
            if (&*F->getEntryBlock().begin() == last->loc) return std::format("{} (Function \"{}\" entrypoint)", ContractPassUtility::getInstrLocStr(F, false), F->getName().str());
            return ContractPassUtility::getInstrLocStr(last->loc, false);
        }
};

} // namespace InteractiveSession
