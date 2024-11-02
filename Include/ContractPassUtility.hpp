#pragma once

#include <functional>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <map>
#include <stack>
#include <vector>

using namespace llvm;

/*
 * Apply worklist algorithm
 * Need Start param to make sure that the initialization of parameters does not count as write operation
 * Start param should be the corresponding DbgDeclareInst. It is therefore also not counted.
 */
template <typename T>
std::map<const Instruction*, T> GenericWorklist(const Instruction* Start, std::function<T(T,const Instruction*,void*)> transfer, std::function<T(T,T,const Instruction*,void*)> merge, void* data, T init) {
    std::map<const Instruction*, T> postAccess;
    std::vector<std::tuple<const Instruction*, T, std::stack<const CallBase*>>> todoList = { {Start->getNextNonDebugInstruction(), init, {}} };
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
