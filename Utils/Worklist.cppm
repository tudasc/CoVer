export module Worklist;
import std;
import LLVMModule;
import ContractPassUtility;

using namespace llvm;

namespace Worklist {

export template<typename T>
using TransferFunction = std::function<T(T,const Instruction*,void*)>;
export template<typename T>
using MergeFunction = std::function<std::pair<T,bool>(T const&,T const&,const Instruction*,void*)>;
export template<typename T>
using CallReturnFunction = std::function<T(T,T,const CallBase*,void*)>;

export enum struct TraceKind { LINEAR, BRANCH, SWITCH, FUNCENTRY, FUNCEXIT };
export template<typename T>
struct JumpTraceEntry {
    JumpTraceEntry<T>() {}
    JumpTraceEntry<T>(T ai, Instruction* I, TraceKind k, std::vector<JumpTraceEntry<T>*> preds) : analysisInfo{ai}, loc{I}, kind{k}, predecessors{preds} {}
    T analysisInfo{};
    Instruction* loc = nullptr;
    TraceKind kind = TraceKind::LINEAR;
    std::vector<JumpTraceEntry<T>*> predecessors;
    bool operator<(const JumpTraceEntry<T> other) const { return loc < other.loc; };
};
export template<typename T>
struct TraceDB : std::shared_ptr<DenseMap<Instruction*, std::unique_ptr<JumpTraceEntry<T>>>> {
    TraceDB<T>() : std::shared_ptr<DenseMap<Instruction*, std::unique_ptr<JumpTraceEntry<T>>>>(std::make_shared<DenseMap<Instruction*, std::unique_ptr<JumpTraceEntry<T>>>>()) {}
    JumpTraceEntry<T>* operator[](Instruction* I) const {
        std::unique_ptr<JumpTraceEntry<T>>& entry = (**this)[I];
        if (!entry) entry = std::make_unique<JumpTraceEntry<T>>();
        return entry.get();
    }
};
export template<typename T>
struct WorklistResult {
    DenseMap<Instruction*, T> AnalysisInfo;
    TraceDB<T> JumpTraces;
    std::function<std::string(T)> AnalysisInfoToStr = nullptr;
};

// Instruction executed after I in the same function. For invokes, this is the start of the normal destination
Instruction* getContinuation(Instruction* I) {
    if (InvokeInst* II = dyn_cast<InvokeInst>(I)) return &II->getNormalDest()->front();
    return I->getNextNode();
}

// Trace entry holding the info after I, i.e. that of its continuation. Falls back to I itself if the continuation was not traced, null if neither was
export template<typename T>
JumpTraceEntry<T>* getTraceAfter(TraceDB<T> const& trace, Instruction* I) {
    if (Instruction* next = getContinuation(I); next && trace->contains(next)) return trace[next];
    if (trace->contains(I)) return trace[I];
    return nullptr;
}

template<typename T>
void updateJumpTrace(TraceDB<T> const& trace, Instruction* cur, Instruction* prev, TraceKind kind, T const& analysisInfo) {
    if (trace->contains(cur)) {
        if (std::find(trace[cur]->predecessors.begin(), trace[cur]->predecessors.end(), trace[prev]) == trace[cur]->predecessors.end())
            trace[cur]->predecessors.push_back(trace[prev]);
        trace[cur]->analysisInfo = analysisInfo;
    } else {
        std::vector<JumpTraceEntry<T>*> preds;
        if (trace->contains(prev)) preds.push_back(trace[prev]);
        *trace[cur] = JumpTraceEntry<T>(analysisInfo, cur, kind, preds);
    }
}

template <typename T>
bool getMergeResult(DenseMap<Instruction*, T>& AI, TraceDB<T> const& trace, T&& prevInfo, MergeFunction<T> const& merge, Instruction* cur, void* data) {
    bool resume = true;
    auto [it, inserted] = AI.try_emplace(cur, std::move(prevInfo));
    if (!inserted) {
        std::tie(it->second, resume) = merge(prevInfo, it->second, cur, data);
    }
    if (trace->contains(cur)) trace[cur]->analysisInfo = it->second;
    return resume;
}

template<typename T>
struct WorklistEntry {
    Instruction* start;
    T initial;
    std::stack<CallBase*> stack;
};

/*
 * Apply worklist algorithm
 * Need Start param to make sure that the initialization of parameters does not count as operation
 */
export template <typename T>
WorklistResult<T> GenericWorklist(Instruction* Start, bool transferFirst, TransferFunction<T> transfer, MergeFunction<T> merge, void* data, T init, CallReturnFunction<T> callReturn = nullptr) {
    // Jumptrace
    TraceDB<T> jumptraces;
    // Analysis Info mapping
    DenseMap<Instruction*, T> postAccess;
    // Worklist
    std::queue<WorklistEntry<T>> todoList;

    if (!transferFirst) {
        updateJumpTrace(jumptraces, Start, nullptr, TraceKind::LINEAR, init);
        todoList.push({getContinuation(Start), init, {}});
        updateJumpTrace(jumptraces, getContinuation(Start), Start, TraceKind::LINEAR, init);
    } else {
        todoList.push({Start, init, {}});
        updateJumpTrace(jumptraces, Start, nullptr, TraceKind::LINEAR, init);
    }

    // Map of OpenMP functions to index with function pointer
    static const std::map<StringRef,int> OMPNames = {{"__kmpc_omp_task_alloc", 5}, {"__kmpc_fork_call", 2}};

    // Start of worklist algorithm
    while (!todoList.empty()) {
        auto [cur, prevInfo, stack] = std::move(todoList.front());

        while (cur != nullptr) {
            // Add previous info depending on following conditions:
            // 1. In any case, prevInfo MUST have the corresponding access
            // 2. Either: It is a "may" analysis, "next" was until now unreachable, or it was reached before and already contains the access
            // If those apply, add that access. Otherwise, remove it if present (=> "must" analysis, node reached with info, jumped here without)
            if (!getMergeResult(postAccess, jumptraces, std::move(prevInfo), merge, cur, data)) {
                // Already visited and analysis does not wish to pursue further.
                // Remove from worklist, or pop stack
                Instruction* stopped = cur;
                Instruction* tmpNext = nullptr;
                CallBase* tmpCall = nullptr;
                while (!tmpNext && !stack.empty()) {
                    tmpCall = stack.top();
                    tmpNext = getContinuation(tmpCall);
                    stack.pop();
                }
                // Either tmpNext is set, or null because tail-call stack end or stack was empty
                if (!tmpNext) break;
                cur = tmpNext;
                if (callReturn) {
                    // Callee info is shared by all callsites. Continue with the info of this callsite instead,
                    // combined with the callee info at its returns (or here, if no return was reached yet)
                    T calleeInfo = postAccess[stopped];
                    bool foundRet = false;
                    if (Function* Callee = ContractPassUtility::getCalleeThroughAlias(tmpCall)) {
                        for (BasicBlock& BB : *Callee) {
                            auto it = postAccess.find(BB.getTerminator());
                            if (!isa<ReturnInst>(BB.getTerminator()) || it == postAccess.end()) continue;
                            calleeInfo = foundRet ? merge(it->second, calleeInfo, it->first, data).first : it->second;
                            foundRet = true;
                        }
                    }
                    prevInfo = callReturn(postAccess[tmpCall], calleeInfo, tmpCall, data);
                    continue;
                }
            }

            // Call transfer function
            postAccess[cur] = transfer(std::move(postAccess[cur]), cur, data);
            prevInfo = postAccess[cur];

            // Check for branching / terminating instructions
            // Invokes are handled as calls below, continuing at the normal destination
            if (cur->isTerminator() && !isa<CallBase>(cur)) {
                TraceKind branchKind = isa<SwitchInst>(cur) ? TraceKind::SWITCH : TraceKind::BRANCH;
                for (BasicBlock* alt : cur->successors()) {
                    updateJumpTrace(jumptraces, &alt->front(), cur, branchKind, prevInfo);
                    todoList.push( {&alt->front(), prevInfo, stack} );
                }
            }

            // Check if function call: If it is, jump to function body
            // If not, continue with normal next instruction
            Instruction* next = nullptr;
            TraceKind next_trace_entry;
            if (CallBase* CB = dyn_cast<CallBase>(cur)) {
                Function* Callee = ContractPassUtility::getCalleeThroughAlias(CB);
                if (Callee && (OMPNames.contains(Callee->getName()) || !Callee->isDeclaration())) {
                    stack.push(CB);
                    if (!OMPNames.contains(Callee->getName())) {
                        next = &Callee->getEntryBlock().front();
                        next_trace_entry = TraceKind::FUNCENTRY;
                    } else {
                        if (Function* ompFunc = dyn_cast<Function>(CB->getArgOperand(OMPNames.at(Callee->getName())))) {
                            if (!ompFunc->isDeclaration())
                                next = &ompFunc->getEntryBlock().front();
                        }
                        if (!next) {
                            errs() << "NOTE: Could not resolve OpenMP outlined call! Verification accuracy is impaired\n";
                            stack.pop();
                        }
                    }
                } else {
                    // Check for annotations
                    std::set<Function*> annots = ContractPassUtility::getFPAnnots(CB);
                    bool foundnext = false;
                    for (Function* F : annots) {
                        if (F->isDeclaration()) continue;
                        std::stack<CallBase*> new_stack = stack;
                        new_stack.push(CB);
                        updateJumpTrace(jumptraces, &F->getEntryBlock().front(), cur, TraceKind::FUNCENTRY, prevInfo);
                        todoList.push( {&F->getEntryBlock().front(), prevInfo, std::move(new_stack)} );
                        foundnext = true;
                    }
                    if (foundnext) goto next_iter;
                }
            }
            if (!next && !isa<ReturnInst>(cur)) {
                next = getContinuation(cur);
                if (next) next_trace_entry = TraceKind::LINEAR;
            }

            // Check if returning from function
            if (isa<ReturnInst>(cur) && !stack.empty()) {
                // Forward to next from stack
                // Callee info is shared by all callsites, so combine it with the info of this callsite
                if (callReturn) prevInfo = callReturn(postAccess[stack.top()], prevInfo, stack.top(), data);
                next = getContinuation(stack.top());
                stack.pop();
                next_trace_entry = TraceKind::FUNCEXIT;
            } else if (isa<ReturnInst>(cur)) {
                // Stack is empty. But if we started inside a function, context includes all callsites
                Function* func = cur->getFunction();
                std::vector<User*> callers(func->user_begin(), func->user_end());
                // Add aliases of function as potential callers as well
                for (User* U : func->users()) {
                    if (GlobalAlias* GA = dyn_cast<GlobalAlias>(U)) callers.insert(callers.end(), GA->user_begin(), GA->user_end());
                }
                for (User* U : callers) {
                    if (CallBase* CB = dyn_cast<CallBase>(U)) {
                        if (ContractPassUtility::getCalleeThroughAlias(CB) != func) continue;
                        // Add callsite next to todoList
                        todoList.push( {getContinuation(CB), prevInfo, stack} );
                    }
                }
            }

            // Update traces
            if (next) {
                updateJumpTrace(jumptraces, next, cur, next_trace_entry, postAccess[cur]);
            }

            // Know next instruction, continue loop or iter is null and we are done
            cur = next;
        }
        next_iter:
        todoList.pop();
    }
    return {std::move(postAccess), jumptraces, nullptr};
}

}
