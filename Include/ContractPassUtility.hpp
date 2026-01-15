#pragma once

#include "ContractTree.hpp"
#include "ErrorMessage.h"
#include <functional>
#include <llvm/ADT/StringRef.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <optional>
#include <sys/types.h>
#include <tuple>
#include <vector>

using namespace llvm;

#define DEBUG_ENV "COVER_LLVM_DEBUG"
#define IS_DEBUG (getenv(DEBUG_ENV) != NULL && atoi(getenv(DEBUG_ENV)) == 1)

namespace ContractPassUtility {
    template<typename T>
    using TransferFunction = std::function<T(T,const Instruction*,void*)>;
    template<typename T>
    using MergeFunction = std::function<std::pair<T,bool>(T,T,const Instruction*,void*)>;

    enum struct TraceKind { LINEAR, BRANCH, FUNCENTRY, FUNCEXIT };
    template<typename T>
    struct JumpTraceEntry {
        JumpTraceEntry<T>() {}
        JumpTraceEntry<T>(T ai, Instruction* I, TraceKind k, std::vector<JumpTraceEntry<T>*> preds) : analysisInfo{ai}, loc{I}, kind{k}, predecessors{preds} {}
        T analysisInfo;
        Instruction* loc;
        TraceKind kind;
        std::vector<JumpTraceEntry<T>*> predecessors;
        bool operator<(const JumpTraceEntry<T> other) const { return loc < other.loc; };
    };
    template<typename T>
    struct TraceDB : std::shared_ptr<std::map<Instruction*, JumpTraceEntry<T>>> {
        TraceDB<T>() : std::shared_ptr<std::map<Instruction*, JumpTraceEntry<T>>>(std::make_shared<std::map<Instruction*, JumpTraceEntry<T>>>()) {}
        JumpTraceEntry<T>* operator[](Instruction* I) const { return &(**this)[I]; }
    };
    template<typename T>
    struct WorklistResult : GenericWLRes {
        std::map<Instruction*, T> AnalysisInfo;
        TraceDB<T> JumpTraces;
        std::function<std::string(T)> AnalysisInfoToStr = nullptr;
    };

    /*
    * Apply worklist algorithm
    * Need Start param to make sure that the initialization of parameters does not count as operation
    */
    template <typename T>
    WorklistResult<T> GenericWorklist(Instruction* Start,  TransferFunction<T> transfer, MergeFunction<T> merge, void* data, T init);

    /*
    * Get line number, or get a string representation of the location
    * Format: <module>:<line> or UNKNOWN depending on output of getLineNumber
    */
    std::optional<uint> getLineNumber(const Instruction* I);
    std::string getInstrLocStr(const Instruction* I, bool longform = true);
    std::string getInstrLocStr(const Function* F, bool longform = true);
    FileReference getFileReference(const Instruction* I);

    /*
    * Check if call applies to target (which may be a tag or function name)
    */
    bool checkCalledApplies(const CallBase* CB, const std::string Target, bool isTag, std::map<const Function*, std::vector<ContractTree::TagUnit>> Tags);

    /*
    * Check if contract and call parameter fit
    */
    bool checkParamMatch(const Value* contrP, const Value* callP, ContractTree::ParamAccess acc);

    /*
    * Check if two calls match by contract definition
    */
    bool checkCallParamApplies(const CallBase* Source, const CallBase* Target, const std::string TargetStr, ContractTree::CallParam const& P, std::map<const Function*, std::vector<ContractTree::TagUnit>> Tags);
};

extern std::map<const Function*, std::set<CallBase*>> AnnotFuncReverse;

template<typename T>
struct WorklistEntry {
    Instruction* start;
    T initial;
    std::stack<CallBase*> stack;
};

template<typename T>
void updateJumpTrace(ContractPassUtility::TraceDB<T> trace, Instruction* cur, Instruction* prev, ContractPassUtility::TraceKind kind, T analysisInfo) {
    if (trace->contains(cur)) {
        if (std::find(trace[cur]->predecessors.begin(), trace[cur]->predecessors.end(), trace[prev]) == trace[cur]->predecessors.end())
            trace[cur]->predecessors.push_back(trace[prev]);
        trace[cur]->analysisInfo = analysisInfo;
    } else {
        if (trace->contains(prev)) trace->insert({cur, ContractPassUtility::JumpTraceEntry<T>(analysisInfo, cur, kind, {trace[prev]})});
        else trace->insert({cur, ContractPassUtility::JumpTraceEntry<T>(analysisInfo, cur, kind, {})});
    }
}

template <typename T>
std::pair<T, bool> getMergeResult(std::map<Instruction*, T>& AI, ContractPassUtility::TraceDB<T> trace, T prevInfo, std::function<std::pair<T,bool>(T,T,const Instruction*,void*)> merge, Instruction* cur, void* data) {
    bool resume = true;
    T info;
    if (!AI.contains(cur)) {
        info = prevInfo;
    } else {
        // Encountered already. Call merge function
        std::tie(info, resume) = merge(prevInfo, AI[cur], cur, data);
    }
    AI[cur] = info;
    if (trace->contains(cur)) trace[cur]->analysisInfo = info;
    return {info, resume};
}

/*
 * Apply worklist algorithm
 * Need Start param to make sure that the initialization of parameters does not count as operation
 */
template <typename T>
ContractPassUtility::WorklistResult<T> ContractPassUtility::GenericWorklist(Instruction* Start, TransferFunction<T> transfer, MergeFunction<T> merge, void* data, T init) {
    // Jumptrace
    TraceDB<T> jumptraces;
    updateJumpTrace(jumptraces, Start, nullptr, TraceKind::LINEAR, init);

    // Analysis Info mapping
    std::map<Instruction*, T> postAccess;

    // Worklist
    std::queue<WorklistEntry<T>> todoList;
    todoList.push({Start, init, {}});

    // Map of OpenMP functions to index with function pointer
    std::map<StringRef,int> OMPNames = {{"__kmpc_omp_task_alloc", 5}, {"__kmpc_fork_call", 2}};

    // Start of worklist algorithm
    while (!todoList.empty()) {
        Instruction* cur = todoList.front().start;
        T prevInfo = todoList.front().initial;
        std::stack<CallBase*> stack = todoList.front().stack;

        while (cur != nullptr) {
            // Add previous info depending on following conditions:
            // 1. In any case, prevInfo MUST have the corresponding access
            // 2. Either: It is a "may" analysis, "next" was until now unreachable, or it was reached before and already contains the access
            // If those apply, add that access. Otherwise, remove it if present (=> "must" analysis, node reached with info, jumped here without)
            std::pair<T, bool> mergeRes = getMergeResult(postAccess, jumptraces, prevInfo, merge, cur, data);
            if (!mergeRes.second) {
                // Already visited and analysis does not wish to pursue further.
                // Remove from worklist, or pop stack
                Instruction* tmpNext = nullptr;
                while (!tmpNext && !stack.empty()) {
                    tmpNext = stack.top()->getNextNonDebugInstruction();
                    stack.pop();
                }
                // Either tmpNext is set, or null because tail-call stack end or stack was empty
                if (tmpNext) {
                    cur = tmpNext;
                } else break;
            }

            // Call transfer function
            postAccess[cur] = transfer(postAccess[cur], cur, data);
            prevInfo = postAccess[cur];

            // Check for branching / terminating instructions
            // Missing because not sure if needed / relevant / used / too little info / lazy:
            // CleanupReturnInst, CatchReturnInst, CatchSwitchInst, CallBrInst, ResumeInst, InvokeInst, IndirectBrInst
            if (BranchInst* BR = dyn_cast<BranchInst>(cur)) {
                for (BasicBlock* alt : BR->successors()) {
                    updateJumpTrace(jumptraces, &alt->front(), cur, TraceKind::BRANCH, postAccess[cur]);
                    todoList.push( {&alt->front(), postAccess[cur], stack} );
                }
                break;
            }
            if (SwitchInst* SI = dyn_cast<SwitchInst>(cur)) {
                for (int i = 0; i < SI->getNumSuccessors(); i++) {
                    BasicBlock* alt = SI->getSuccessor(i);
                    todoList.push( {&alt->front(), postAccess[cur], stack} );
                }
            }
            if (isa<UnreachableInst>(cur)) {
                break;
            }

            // Check if function call: If it is, jump to function body
            // If not, continue with normal next instruction
            Instruction* next = nullptr;
            TraceKind next_trace_entry;
            if (CallBase* CB = dyn_cast<CallBase>(cur)) {
                if (CB->getCalledFunction() && (OMPNames.contains(CB->getCalledFunction()->getName()) || !CB->getCalledFunction()->isDeclaration())) {
                    stack.push(CB);
                    if (!OMPNames.contains(CB->getCalledFunction()->getName())) {
                        next = &CB->getCalledFunction()->getEntryBlock().front();
                        next_trace_entry = TraceKind::FUNCENTRY;
                    } else {
                        if (Function* ompFunc = dyn_cast<Function>(CB->getArgOperand(OMPNames[CB->getCalledFunction()->getName()]))) {
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
                    if (MDNode* Existing = CB->getMetadata(LLVMContext::MD_annotation)) {
                        MDTuple* Tuple = cast<MDTuple>(Existing);
                        bool foundnext = false;
                        for (MDOperand const& N : Tuple->operands()) {
                            if (isa<MDString>(N.get()) && cast<MDString>(N.get())->getString().starts_with("CoVer_AnnotFP")) {
                                // Funcptr annotation exists! Add to todoList. May be multiple so cant just set next
                                std::string fp_annot = cast<MDString>(N.get())->getString().str();
                                Function* target_func = CB->getModule()->getFunction(fp_annot.substr(fp_annot.find("|") + 1));
                                std::stack<CallBase*> new_stack = stack;
                                new_stack.push(CB);
                                AnnotFuncReverse[target_func].insert(CB);
                                updateJumpTrace(jumptraces, &target_func->getEntryBlock().front(), cur, TraceKind::FUNCENTRY, postAccess[cur]);
                                todoList.push( {&target_func->getEntryBlock().front(), postAccess[cur], new_stack} );
                                foundnext = true;
                            }
                        }
                        if (foundnext) goto next_iter;
                    }
                }
            }
            if (!next) {
                next = cur->getNextNonDebugInstruction();
                if (next) next_trace_entry = TraceKind::LINEAR;
            }

            // Check if returning from function
            if (!next && !stack.empty()) {
                // Forward to next from stack
                next = stack.top()->getNextNonDebugInstruction();
                stack.pop();
                next_trace_entry = TraceKind::FUNCEXIT;
            } else if (!next) {
                // Stack is empty. But if we started inside a function, context includes all callsites
                Function* func = cur->getParent()->getParent();
                for (User* U : func->users()) {
                    if (CallBase* CB = dyn_cast<CallBase>(U)) {
                        // Add callsite next to todoList
                        todoList.push( {CB->getNextNonDebugInstruction(), postAccess[cur], stack} );
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
    return {{}, postAccess, jumptraces, nullptr};
}
