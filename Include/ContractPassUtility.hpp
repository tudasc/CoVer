#pragma once

#include "ContractTree.hpp"
#include <functional>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <map>
#include <string>
#include <optional>
#include <stack>
#include <sys/types.h>
#include <vector>

using namespace llvm;

#define DEBUG_ENV "LLVMCONTRACTS_DEBUG"
#define IS_DEBUG (getenv(DEBUG_ENV) != NULL && atoi(getenv(DEBUG_ENV)) == 1)

/*
 * Apply worklist algorithm
 * Need Start param to make sure that the initialization of parameters does not count as operation
 */
template <typename T>
std::map<const Instruction*, T> GenericWorklist(const Instruction* Start, std::function<T(T,const Instruction*,void*)> transfer, std::function<T(T,T,const Instruction*,void*)> merge, void* data, T init) {
    std::map<const Instruction*, T> postAccess;
    std::vector<std::tuple<const Instruction*, T, std::stack<const CallBase*>>> todoList = { {Start, init, {}} };
    while (!todoList.empty()) {
        const Instruction* next = std::get<0>(*todoList.begin());
        T prevInfo = std::get<1>(*todoList.begin());
        std::stack<const CallBase*>& stack = std::get<2>(*todoList.begin());

        while (next != nullptr) {
            // Add previous info depending on following conditions:
            // 1. In any case, prevInfo MUST have the corresponding access
            // 2. Either: It is a "may" analysis, "next" was until now unreachable, or it was reached before and already contains the access
            // If those apply, add that access. Otherwise, remove it if present (=> "must" analysis, node reached with info, jumped here without)
            if (!postAccess.contains(next)) {
                postAccess[next] = prevInfo;
            } else {
                // Encountered already. Call merge function
                T newInfo = merge(prevInfo, postAccess[next], next, data);
                postAccess[next] = newInfo;
            }

            // Call transfer function
            postAccess[next] = transfer(postAccess[next], next, data);

            // Check for branching / terminating instructions
            // Missing because not sure if needed / relevant / used / too little info / lazy:
            // CleanupReturnInst, CatchReturnInst, CatchSwitchInst, CallBrInst, ResumeInst, InvokeInst, IndirectBrInst
            #warning TODO SwitchInst
            if (const BranchInst* BR = dyn_cast<BranchInst>(next)) {
                for (const BasicBlock* alt : BR->successors())
                    todoList.push_back( {&alt->front(), postAccess[next], stack} );
            }
            if (isa<ReturnInst>(next) || isa<UnreachableInst>(next)) {
                break;
            }

            // Update for next iteration
            prevInfo = postAccess[next];

            // Check if function call: If it is, jump to function body
            // If not, continue with normal next instruction
            if (const CallBase* CB = dyn_cast<CallBase>(next)) {
                if (CB->getCalledFunction()) {
                    stack.push(CB);
                    next = &CB->getCalledFunction()->getEntryBlock().front();
                }
            } else {
                next = next->getNextNonDebugInstruction();
            }

            // Check if returning from function
            if (!stack.empty() && !next) {
                // Forward to next from stack
                next = stack.top()->getNextNonDebugInstruction();
                stack.pop();
            }
        }
        todoList.erase(todoList.begin());
    }
    return postAccess;
}

inline std::optional<uint> getLineNumber(const Instruction* I) {
    if (const DebugLoc& N = I->getDebugLoc()) {
        return N.getLine();
    }
    return std::nullopt;
}
inline std::string getInstrLocStr(const Instruction* I) {
    return demangle(I->getParent()->getParent()->getName()) + ":" + (getLineNumber(I).has_value() ? std::to_string(getLineNumber(I).value()) : "UNKNOWN");
}

inline bool checkCalledApplies(const CallBase* CB, const std::string Target, bool isTag, std::map<const Function*, std::vector<ContractTree::TagUnit>> Tags) {
    if (!isTag) {
        return CB->getCalledFunction()->getName() == Target;
    } else {
        if (!Tags.contains(CB->getCalledFunction())) return false;
        for (const ContractTree::TagUnit tag : Tags[CB->getCalledFunction()]) {
            if (tag.tag == Target) {
                return true;
            }
        }
        return false;
    }
}

inline bool checkCallParamApplies(const CallBase* Source, const CallBase* Target, const std::string TargetStr, ContractTree::CallParam const& P, std::map<const Function*, std::vector<ContractTree::TagUnit>> Tags) {
    const Value* candidateParam;
    if (!P.callPisTagVar) {
        candidateParam = Target->getArgOperand(P.callP);
        if (candidateParam == Source->getArgOperand(P.contrP)) {
            return true;
        }
    } else {
        for (ContractTree::TagUnit TagU : Tags[Target->getCalledFunction()]) {
            if (TagU.tag != TargetStr) continue;
            if (!TagU.param.has_value()) continue;
            candidateParam = Target->getArgOperand(*TagU.param);
            if (candidateParam == Source->getArgOperand(P.contrP)) {
                return true;
            }
        }
    }
    return false;
}
