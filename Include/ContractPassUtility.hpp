#pragma once

#include "ContractTree.hpp"
#include <functional>
#include <llvm/ADT/StringRef.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>
#include <map>
#include <queue>
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
        T analysisInfo;
        const Instruction* loc;
        TraceKind kind;
        std::vector<JumpTraceEntry<T>> predecessors;
    };
    template<typename T>
    struct WorklistResult {
        std::map<const Instruction*, T> AnalysisInfo;
        std::map<const Instruction*, JumpTraceEntry<T>> JumpTraces;
    };

    /*
    * Apply worklist algorithm
    * Need Start param to make sure that the initialization of parameters does not count as operation
    */
    template <typename T>
    WorklistResult<T> GenericWorklist(const Instruction* Start,  TransferFunction<T> transfer, MergeFunction<T> merge, void* data, T init);

    /*
    * Get line number, or get a string representation of the location
    * Format: <module>:<line> or UNKNOWN depending on output of getLineNumber
    */
    std::optional<uint> getLineNumber(const Instruction* I);
    std::string getInstrLocStr(const Instruction* I);
    ErrorReference getErrorReference(const Instruction* I);

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

template<typename T>
struct WorklistEntry {
    const Instruction* start;
    T initial;
    std::stack<const CallBase*> stack;
};

template<typename T>
void updateJumpTrace(std::map<const Instruction*, ContractPassUtility::JumpTraceEntry<T>>& trace, const Instruction* cur, const Instruction* prev, ContractPassUtility::TraceKind kind, T analysisInfo) {
    if (trace.contains(cur)) {
        if (kind != ContractPassUtility::TraceKind::LINEAR) {
            trace[cur].predecessors.push_back(trace[prev]);
        }
        trace[cur].analysisInfo = analysisInfo;
    } else {
        trace[cur] = {analysisInfo, cur, kind, {trace[prev]}};
    }
}

template <typename T>
std::pair<T, bool> getMergeResult(std::map<const Instruction*, T>& AI, T prevInfo, std::function<std::pair<T,bool>(T,T,const Instruction*,void*)> merge, const Instruction* cur, void* data) {
    bool resume = true;
    T info;
    if (!AI.contains(cur)) {
        info = prevInfo;
    } else {
        // Encountered already. Call merge function
        std::tie(info, resume) = merge(prevInfo, AI[cur], cur, data);
    }
    AI[cur] = info;
    return {info, resume};
}

/*
 * Apply worklist algorithm
 * Need Start param to make sure that the initialization of parameters does not count as operation
 */
template <typename T>
ContractPassUtility::WorklistResult<T> ContractPassUtility::GenericWorklist(const Instruction* Start, TransferFunction<T> transfer, MergeFunction<T> merge, void* data, T init) {
    // Jumptrace
    std::map<const Instruction*, JumpTraceEntry<T>> jumptraces;
    jumptraces[Start] = {init, Start, {}};

    // Analysis Info mapping
    std::map<const Instruction*, T> postAccess;

    // Worklist
    std::queue<WorklistEntry<T>> todoList;
    todoList.push({Start, init, {}});

    // Map of OpenMP functions to index with function pointer
    std::map<StringRef,int> OMPNames = {{"__kmpc_omp_task_alloc", 5}, {"__kmpc_fork_call", 2}};

    // Start of worklist algorithm
    while (!todoList.empty()) {
        const Instruction* cur = todoList.front().start;
        T prevInfo = todoList.front().initial;
        std::stack<const CallBase*> stack = todoList.front().stack;

        while (cur != nullptr) {
            // Add previous info depending on following conditions:
            // 1. In any case, prevInfo MUST have the corresponding access
            // 2. Either: It is a "may" analysis, "next" was until now unreachable, or it was reached before and already contains the access
            // If those apply, add that access. Otherwise, remove it if present (=> "must" analysis, node reached with info, jumped here without)
            std::pair<T, bool> mergeRes = getMergeResult(postAccess, prevInfo, merge, cur, data);
            if (!mergeRes.second) {
                // Already visited and analysis does not wish to pursue further.
                // Remove from worklist, or pop stack
                const Instruction* tmpNext = nullptr;
                while (!tmpNext) {
                    if (stack.empty()) break;
                    tmpNext = stack.top()->getNextNonDebugInstruction();
                    stack.pop();
                }
                // Either tmpNext is set, or null because tail-call stack end or stack was empty
                if (tmpNext)
                    cur = tmpNext;
                else
                    break;
            }

            // Call transfer function
            postAccess[cur] = transfer(postAccess[cur], cur, data);

            // Check for branching / terminating instructions
            // Missing because not sure if needed / relevant / used / too little info / lazy:
            // CleanupReturnInst, CatchReturnInst, CatchSwitchInst, CallBrInst, ResumeInst, InvokeInst, IndirectBrInst
            if (const BranchInst* BR = dyn_cast<BranchInst>(cur)) {
                for (const BasicBlock* alt : BR->successors()) {
                    updateJumpTrace(jumptraces, &alt->front(), cur, TraceKind::BRANCH, postAccess[cur]);
                    todoList.push( {&alt->front(), postAccess[cur], stack} );
                }
            }
            if (const SwitchInst* SI = dyn_cast<SwitchInst>(cur)) {
                for (int i = 0; i < SI->getNumSuccessors(); i++) {
                    const BasicBlock* alt = SI->getSuccessor(i);
                    todoList.push( {&alt->front(), postAccess[cur], stack} );
                }
            }
            if (isa<UnreachableInst>(cur)) {
                break;
            }

            // Update for next iteration
            prevInfo = postAccess[cur];

            // Check if function call: If it is, jump to function body
            // If not, continue with normal next instruction
            const Instruction* next = nullptr;
            if (const CallBase* CB = dyn_cast<CallBase>(cur)) {
                if (CB->getCalledFunction() && (OMPNames.contains(CB->getCalledFunction()->getName()) || !CB->getCalledFunction()->isDeclaration())) {
                    stack.push(CB);
                    if (!OMPNames.contains(CB->getCalledFunction()->getName())) {
                        next = &CB->getCalledFunction()->getEntryBlock().front();
                    } else {
                        if (const Function* ompFunc = dyn_cast<Function>(CB->getArgOperand(OMPNames[CB->getCalledFunction()->getName()]))) {
                            if (!ompFunc->isDeclaration())
                                next = &ompFunc->getEntryBlock().front();
                        }
                        if (!next) {
                            errs() << "NOTE: Could not resolve OpenMP outlined call! Verification accuracy is impaired\n";
                            stack.pop();
                        }
                    }
                }
            }
            if (!next) {
                next = cur->getNextNonDebugInstruction();
                if (next) updateJumpTrace(jumptraces, next, cur, TraceKind::LINEAR, prevInfo);
            }

            // Check if returning from function
            if (!next && !stack.empty()) {
                // Forward to next from stack
                next = stack.top()->getNextNonDebugInstruction();
                stack.pop();
            } else if (!next) {
                // Stack is empty. But if we started inside a function, context includes all callsites
                const Function* func = cur->getParent()->getParent();
                for (const User* U : func->users()) {
                    if (const CallBase* CB = dyn_cast<CallBase>(U)) {
                        // Add callsite next to todoList
                        todoList.push( {CB->getNextNonDebugInstruction(), postAccess[cur], stack} );
                    }
                }
            }

            // Know next instruction, continue loop or iter is null and we are done
            cur = next;
        }
        todoList.pop();
    }
    return {postAccess, jumptraces};
}
