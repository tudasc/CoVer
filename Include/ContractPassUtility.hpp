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
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>
#include <map>
#include <stack>
#include <string>
#include <optional>
#include <sys/types.h>
#include <vector>

using namespace llvm;

#define DEBUG_ENV "COVER_LLVM_DEBUG"
#define IS_DEBUG (getenv(DEBUG_ENV) != NULL && atoi(getenv(DEBUG_ENV)) == 1)

namespace ContractPassUtility {
    template<typename T>
    using TransferFunction = std::function<T(T,const Instruction*,void*)>;
    template<typename T>
    using MergeFunction = std::function<std::pair<T,bool>(T,T,const Instruction*,void*)>;
    /*
    * Apply worklist algorithm
    * Need Start param to make sure that the initialization of parameters does not count as operation
    */
    template <typename T>
    std::map<const Instruction*, T> GenericWorklist(const Instruction* Start, TransferFunction<T> transfer, MergeFunction<T> merge, void* data, T init);

    /*
    * Get line number, or get a string representation of the location
    * Format: <module>:<line> or UNKNOWN depending on output of getLineNumber
    */
    std::optional<uint> getLineNumber(const Instruction* I);
    std::string getInstrLocStr(const Instruction* I);
    FileReference getFileReference(const Instruction* I);

    /*
    * Check if call applies to target (which may be a tag or function name)
    */
    bool checkCalledApplies(const CallBase* CB, const std::string Target, bool isTag, std::map<Function*, std::vector<ContractTree::TagUnit>> Tags);

    /*
    * Check if contract and call parameter fit
    */
    bool checkParamMatch(const Value* contrP, const Value* callP, ContractTree::ParamAccess acc);

    /*
    * Check if two calls match by contract definition
    */
    bool checkCallParamApplies(const CallBase* Source, const CallBase* Target, const std::string TargetStr, ContractTree::CallParam const& P, std::map<Function*, std::vector<ContractTree::TagUnit>> Tags);
};

template<typename T>
struct WorklistEntry {
    const Instruction* start;
    T initial;
    std::stack<const CallBase*> stack;
};

/*
 * Apply worklist algorithm
 * Need Start param to make sure that the initialization of parameters does not count as operation
 */
template <typename T>
std::map<const Instruction*, T> ContractPassUtility::GenericWorklist(const Instruction* Start, TransferFunction<T> transfer, MergeFunction<T> merge, void* data, T init) {
    // Analysis Info mapping
    std::map<const Instruction*, T> postAccess;

    // Worklist
    std::vector<WorklistEntry<T>> todoList = { {Start, init, {}} };

    // Map of OpenMP functions to index with function pointer
    std::map<StringRef,int> OMPNames = {{"__kmpc_omp_task_alloc", 5}, {"__kmpc_fork_call", 2}};

    // Start of worklist algorithm
    while (!todoList.empty()) {
        const Instruction* next = todoList[0].start;
        T prevInfo = todoList[0].initial;
        std::stack<const CallBase*> stack = todoList[0].stack;

        while (next != nullptr) {
            // Add previous info depending on following conditions:
            // 1. In any case, prevInfo MUST have the corresponding access
            // 2. Either: It is a "may" analysis, "next" was until now unreachable, or it was reached before and already contains the access
            // If those apply, add that access. Otherwise, remove it if present (=> "must" analysis, node reached with info, jumped here without)
            if (!postAccess.contains(next)) {
                postAccess[next] = prevInfo;
            } else {
                // Encountered already. Call merge function
                std::pair<T,bool> mergeRes = merge(prevInfo, postAccess[next], next, data);
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
                        next = tmpNext;
                    else
                        break;
                }
                postAccess[next] = mergeRes.first;
            }

            // Call transfer function
            postAccess[next] = transfer(postAccess[next], next, data);

            // Check for branching / terminating instructions
            // Missing because not sure if needed / relevant / used / too little info / lazy:
            // CleanupReturnInst, CatchReturnInst, CatchSwitchInst, CallBrInst, ResumeInst, InvokeInst, IndirectBrInst
            if (const BranchInst* BR = dyn_cast<BranchInst>(next)) {
                for (const BasicBlock* alt : BR->successors())
                    todoList.push_back( {&alt->front(), postAccess[next], stack} );
            }
            if (const SwitchInst* SI = dyn_cast<SwitchInst>(next)) {
                for (int i = 0; i < SI->getNumSuccessors(); i++) {
                    const BasicBlock* alt = SI->getSuccessor(i);
                    todoList.push_back( {&alt->front(), postAccess[next], stack} );
                }
            }
            if (isa<UnreachableInst>(next)) {
                break;
            }

            // Update for next iteration
            prevInfo = postAccess[next];

            // Check if function call: If it is, jump to function body
            // If not, continue with normal next instruction
            const Instruction* iter = nullptr;
            if (const CallBase* CB = dyn_cast<CallBase>(next)) {
                if (CB->getCalledFunction() && (OMPNames.contains(CB->getCalledFunction()->getName()) || !CB->getCalledFunction()->isDeclaration())) {
                    stack.push(CB);
                    if (!OMPNames.contains(CB->getCalledFunction()->getName())) {
                        iter = &CB->getCalledFunction()->getEntryBlock().front();
                    } else {
                        if (const Function* ompFunc = dyn_cast<Function>(CB->getArgOperand(OMPNames[CB->getCalledFunction()->getName()]))) {
                            if (!ompFunc->isDeclaration())
                                iter = &ompFunc->getEntryBlock().front();
                        }
                        if (!iter) {
                            errs() << "NOTE: Could not resolve OpenMP outlined call! Verification accuracy is impaired\n";
                            stack.pop();
                        }
                    }
                }
            }
            if (!iter) {
                iter = next->getNextNonDebugInstruction();
            }

            // Check if returning from function
            if (!iter && !stack.empty()) {
                // Forward to next from stack
                iter = stack.top()->getNextNonDebugInstruction();
                stack.pop();
            } else if (!iter) {
                // Stack is empty. But if we started inside a function, context includes all callsites
                const Function* func = next->getParent()->getParent();
                for (const User* U : func->users()) {
                    if (const CallBase* CB = dyn_cast<CallBase>(U)) {
                        // Add callsite next to todoList
                        todoList.push_back( {CB->getNextNonDebugInstruction(), postAccess[next], stack} );
                    }
                }
            }

            // Know next instruction, continue loop or iter is null and we are done
            next = iter;
        }
        todoList.erase(todoList.begin());
    }
    return postAccess;
}
