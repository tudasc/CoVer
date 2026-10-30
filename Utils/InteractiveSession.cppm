module;

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
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
using InteractiveUtils::TraceBlock;

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

// Command history of a previous run, if this is a reanalysis
History LoadHistoryCache() {
    History input_history;
    if (std::filesystem::exists("CoVer_HistoryCache")) {
        std::string line;
        std::ifstream historyfile("CoVer_HistoryCache");
        std::getline(historyfile, line);
        while (std::getline(historyfile, line)) {
            if (line == "|| DIFF HISTORY END ||") break;
            CmdResultCode code = (CmdResultCode)std::atoi(line.substr(0, line.find_first_of("|")).c_str());
            input_history.push_back({line.substr(line.find_first_of("|") + 1), {code, ""}});
        }
        std::filesystem::remove("CoVer_HistoryCache");
    }
    return input_history;
}

void SaveHistoryCache(History const& history) {
    std::ofstream difffile("CoVer_HistoryCache");
    difffile << "|| DIFF HISTORY BEGIN ||\n";
    for (std::pair<std::string, CmdResult> const& x : history) {
        difffile << (int)x.second.code << "|" << x.first << "\n";
    }
    difffile << "|| DIFF HISTORY END ||\n---\n";
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
    std::optional<int> jump_to;
    bool expanded;
    ContractPassUtility::TraceKind kind; // How the last entry is reached from its children
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
        void SaveHistory() const { SaveHistoryCache(history); }

    protected:
        bool expand_as_source = false;
        History history = LoadHistoryCache();
};

export template<typename T>
class TraceSessionImpl : public TraceSession {
    public:
        TraceSessionImpl(TraceDB<T> traceDB, JumpTraceEntry<T>* trace, std::function<std::string(T)> infoToStr) : traceDB{traceDB}, trace{trace}, infoToStr{infoToStr} {
            Refresh();
        }

        void Refresh() override {
            trace_by_blocks = InteractiveUtils::GetTraceList(traceDB, trace, infoToStr, sibling_select);
            InteractiveUtils::WriteReanalyseModule(trace->loc->getModule());
            blocks.clear();
            for (int i = 0; i < trace_by_blocks.size(); i++) {
                TraceBlock<T> const& block = trace_by_blocks[i];
                TraceBlockView view = {
                    .index = i,
                    .first = EntryView(block.first_entry),
                    .description = block.trace_list,
                    .start_loc = block.start_loc,
                    .end_loc = block.end_loc,
                    .jump_to = block.jump_to,
                    .expanded = expanded.contains(block.last_entry),
                    .kind = block.last_entry->kind,
                };
                if (!block.jump_to) {
                    view.selected_child = sibling_select.contains(block.last_entry) ? sibling_select[block.last_entry] : 0;
                    for (JumpTraceEntry<T>* child : block.last_entry->predecessors) view.children.push_back(EntryView(child));
                }
                blocks.push_back(view);
            }
        }

        std::vector<TraceBlockView> const& Blocks() const override { return blocks; }

        std::vector<TraceEntryView> BlockEntries(int block) const override {
            return InteractiveUtils::getBlockEntries(trace_by_blocks[block]) | std::views::transform([this](JumpTraceEntry<T>* entry) { return EntryView(entry); })
                 | std::ranges::to<std::vector>();
        }

        int NumChildren(int block) const override { return trace_by_blocks[block].last_entry->predecessors.size(); }

        void SelectChild(int block, int child) override { sibling_select[trace_by_blocks[block].last_entry] = child; }

        void SetExpanded(int block, bool expand) override {
            if (expand) expanded.insert(trace_by_blocks[block].last_entry);
            else expanded.erase(trace_by_blocks[block].last_entry);
        }

    private:
        TraceDB<T> traceDB;
        JumpTraceEntry<T>* trace;
        std::function<std::string(T)> infoToStr;
        std::map<JumpTraceEntry<T>*,int> sibling_select;
        std::set<JumpTraceEntry<T>*> expanded;
        std::vector<TraceBlock<T>> trace_by_blocks;
        std::vector<TraceBlockView> blocks;

        TraceEntryView EntryView(JumpTraceEntry<T> const* entry) const { return {entry->loc, infoToStr(entry->analysisInfo)}; }
};

} // namespace InteractiveSession
