#pragma once

#include <functional>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <map>

using namespace llvm;

/*
 * Apply worklist algorithm
 * Need Start param to make sure that the initialization of parameters does not count as write operation
 * Start param should be the corresponding DbgDeclareInst. It is therefore also not counted.
 */
template <typename T>
std::map<const Instruction*, T> GenericWorklist(const Instruction* Start, std::function<T(T,const Instruction*,void*)> transfer, std::function<T(T,T,const Instruction*,void*)> merge, void* data, T init) {
    std::map<const Instruction*, T> postAccess;
    std::vector<std::pair<const Instruction*, T>> todoList = { {Start->getNextNonDebugInstruction(), init } };
    while (!todoList.empty()) {
        const Instruction* next = todoList.begin()->first;
        T prevInfo = todoList.begin()->second;

        while (next != nullptr) {
            // Add previous info depending on following conditions:
            // 1. In any case, prevInfo MUST have the corresponding access
            // 2. Either: It is a "may" analysis, "next" was until now unreachable, or it was reached before and already contains the access
            // If those apply, add that access. Otherwise, remove it if present (=> "must" analysis, node reached with info, jumped here without)
            if (!postAccess.contains(next)) {
                postAccess[next] = prevInfo;
            } else {
                // Encountered already. Call merge function
                postAccess[next] = merge(prevInfo, postAccess[next], next, data);
            }

            // Call transfer function
            postAccess[next] = transfer(postAccess[next], next, data);

            // Check for branching / terminating instructions
            // Missing because not sure if needed / relevant / used / too little info / lazy:
            // CleanupReturnInst, CatchReturnInst, CatchSwitchInst, CallBrInst, ResumeInst, InvokeInst, IndirectBrInst
            #warning TODO SwitchInst
            if (const BranchInst* BR = dyn_cast<BranchInst>(next)) {
                for (const BasicBlock* alt : BR->successors())
                    todoList.push_back( {&alt->front(), postAccess[next]} );
            }
            if (isa<ReturnInst>(next) || isa<UnreachableInst>(next)) {
                break;
            }

            // Update for next iteration
            prevInfo = postAccess[next];
            next = next->getNextNonDebugInstruction();
        }
        todoList.erase(todoList.begin());
    }
    return postAccess;
}
