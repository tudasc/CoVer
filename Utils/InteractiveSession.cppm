module;

#include <algorithm>
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
using InteractiveUtils::BlockLine;
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

export struct TraceInstrView {
    int index;
    std::string info;
    std::string ir;
    std::string location;
    std::string value; // Name usable in alias commands, empty if the instruction is no pointer value
    bool indirect_call;
    std::string source; // Source code approximation, empty without debug info
    std::vector<std::string> operands; // Pointer values referenced by the instruction, usable in alias commands
};

export struct TraceChildView {
    int child;
    void const* entry;
    std::string info;
    std::string location;
    std::string function;
};

export struct TraceBlockView {
    int index;
    void const* entry; // Identifies the block across refreshes
    std::string info;
    std::string function;
    std::string description;
    std::string start_loc;
    std::string end_loc;
    std::optional<int> jump_to;
    bool expanded;
    std::string kind; // How the last entry is reached from its children (predecessors)
    std::vector<TraceChildView> children;
    int selected_child = 0;
};

// Views are rebuilt by Refresh, which the trace commands call after every command except for those ending the session
export class TraceSession {
    public:
        virtual ~TraceSession() = default;
        virtual void Refresh() = 0;
        virtual std::vector<TraceBlockView> const& Blocks() const = 0;
        // Instructions of a block, numbered like the instruction arguments of the trace commands
        virtual std::vector<TraceInstrView> const& Instructions(int block) = 0;
        virtual std::vector<BlockLine> BlockLines(int block, bool source) const = 0;

        // Access for the trace commands, block and instruction numbers as in the views
        virtual Instruction* BlockInstruction(int block, int instr) const = 0; // nullptr if there is no such instruction
        virtual Function* BlockFunction(int block) const = 0;
        virtual int NumChildren(int block) const = 0;
        virtual void SelectChild(int block, int child) = 0;
        virtual void SetExpanded(int block, bool expanded) = 0;
        virtual Module* GetModule() const = 0;

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
            InteractiveUtils::WriteReanalyseModule(GetModule());
            instr_cache.clear();
            blocks.clear();
            for (int i = 0; i < trace_by_blocks.size(); i++) {
                TraceBlock<T> const& block = trace_by_blocks[i];
                TraceBlockView view = {
                    .index = i,
                    .entry = block.first_entry,
                    .info = infoToStr(block.first_entry->analysisInfo),
                    .function = block.first_entry->loc->getFunction()->getName().str(),
                    .description = block.trace_list,
                    .start_loc = block.start_loc,
                    .end_loc = block.end_loc,
                    .jump_to = block.jump_to,
                    .expanded = expanded.contains(block.last_entry),
                };
                if (!block.jump_to) {
                    view.kind = InteractiveUtils::traceKindToStr(block.last_entry->kind);
                    view.selected_child = sibling_select.contains(block.last_entry) ? sibling_select[block.last_entry] : 0;
                    for (auto const [k, child] : std::views::enumerate(block.last_entry->predecessors)) {
                        view.children.push_back({static_cast<int>(k), child, infoToStr(child->analysisInfo), ContractPassUtility::getInstrLocStr(child->loc, false), child->loc->getFunction()->getName().str()});
                    }
                }
                blocks.push_back(view);
            }
        }

        std::vector<TraceBlockView> const& Blocks() const override { return blocks; }

        std::vector<TraceInstrView> const& Instructions(int block) override {
            if (!instr_cache.contains(block)) instr_cache[block] = BuildInstructions(block);
            return instr_cache[block];
        }

        std::vector<BlockLine> BlockLines(int block, bool source) const override {
            return InteractiveUtils::getBlockLines(trace_by_blocks[block], source, infoToStr);
        }

        Instruction* BlockInstruction(int block, int instr) const override {
            std::vector<JumpTraceEntry<T>*> entries = InteractiveUtils::getBlockEntries(trace_by_blocks[block]);
            return instr >= 0 && instr < entries.size() ? entries[instr]->loc : nullptr;
        }

        Function* BlockFunction(int block) const override { return trace_by_blocks[block].first_entry->loc->getFunction(); }

        int NumChildren(int block) const override { return trace_by_blocks[block].last_entry->predecessors.size(); }

        void SelectChild(int block, int child) override { sibling_select[trace_by_blocks[block].last_entry] = child; }

        void SetExpanded(int block, bool expand) override {
            if (expand) expanded.insert(trace_by_blocks[block].last_entry);
            else expanded.erase(trace_by_blocks[block].last_entry);
        }

        Module* GetModule() const override { return trace->loc->getModule(); }

    private:
        TraceDB<T> traceDB;
        JumpTraceEntry<T>* trace;
        std::function<std::string(T)> infoToStr;
        std::map<JumpTraceEntry<T>*,int> sibling_select;
        std::set<JumpTraceEntry<T>*> expanded;
        std::vector<TraceBlock<T>> trace_by_blocks;
        std::vector<TraceBlockView> blocks;
        std::map<int, std::vector<TraceInstrView>> instr_cache;

        std::vector<TraceInstrView> BuildInstructions(int b) {
            std::vector<TraceInstrView> instrs;
            TraceBlock<T> const& block = trace_by_blocks[b];
            ModuleSlotTracker MST(block.first_entry->loc->getModule(), false);
            Function const* cur_func = nullptr;
            // Name of a pointer value as resolved by the alias commands (instructions and arguments of the function or named globals), empty if not resolvable
            auto valueName = [&](Value const* V) -> std::string {
                if (!V->getType()->isPointerTy()) return "";
                if (isa<Instruction>(V) || isa<Argument>(V)) {
                    if (V->hasName()) return "%" + V->getName().str();
                    if (MST.getLocalSlot(V) >= 0) return std::format("%{}", MST.getLocalSlot(V));
                }
                if (isa<GlobalVariable>(V) && V->hasName()) return "@" + V->getName().str();
                return "";
            };
            for (auto const [index, entry] : std::views::enumerate(InteractiveUtils::getBlockEntries(block))) {
                Instruction* I = entry->loc;
                if (I->getFunction() != cur_func) {
                    cur_func = I->getFunction();
                    MST.incorporateFunction(*cur_func);
                }
                std::string ir;
                raw_string_ostream strstream(ir);
                I->print(strstream, MST);
                ir.erase(0, ir.find_first_not_of(' '));
                std::string value = valueName(I);
                std::vector<std::string> operands;
                for (Use const& U : I->operands()) {
                    std::string op = valueName(U.get());
                    if (!op.empty() && op != value && !std::ranges::contains(operands, op)) operands.push_back(op);
                }
                CallBase const* CB = dyn_cast<CallBase>(I);
                instrs.push_back({static_cast<int>(index), infoToStr(entry->analysisInfo), ir, ContractPassUtility::getInstrLocStr(I, false), value, CB && CB->isIndirectCall(),
                                  expand_as_source ? InteractiveUtils::getSourceLine(entry, true, infoToStr) : "", operands});
            }
            return instrs;
        }
};

} // namespace InteractiveSession
