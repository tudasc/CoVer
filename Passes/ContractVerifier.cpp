#include "ContractVerifier.hpp"
#include "ContractManager.hpp"

#include <llvm/Analysis/InlineCost.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/WithColor.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Instrumentation.h>
#include <llvm/Demangle/Demangle.h>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "../LangCode/ContractDataVisitor.hpp"

using namespace llvm;
using namespace ContractTree;

PreservedAnalyses ContractVerifierPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    ContractManagerAnalysis::ContractDatabase DB = AM.getResult<ContractManagerAnalysis>(M);

    for (ContractManagerAnalysis::Contract C : DB.Contracts) {
        if (!C.Data.Pre.has_value() && C.Data.Post.has_value()) {
            // No preconditions
            const ContractExpression& Expr = C.Data.Post.value();
            std::string err;
            bool result = false;
            switch (C.Data.Post->OP->type()) {
                case OperationType::READ: {
                    const ReadOperation& rOP = dynamic_cast<const ReadOperation&>(*Expr.OP);
                    result = checkVarRW(rOP.Variable, C.F, true, err).contains(RWStatus::READ);
                    break;
                }
                case OperationType::WRITE: {
                    const WriteOperation& wOP = dynamic_cast<const WriteOperation&>(*Expr.OP);
                    result = checkVarRW(wOP.Variable, C.F, true, err).contains(RWStatus::WRITE);
                    break;
                }
            }
            errs() << "\n";
            if (!err.empty()) {
                errs() << err << "\n";
                continue;
            }
            if (result) {
                WithColor(errs(), HighlightColor::Remark) << "## Contract Fulfilled! ##\n";
                C.Status = ContractManagerAnalysis::FULFILLED;
            } else {
                WithColor(errs(), HighlightColor::Error) << "## Contract violation detected! ##\n";
                C.Status = ContractManagerAnalysis::BROKEN;
            }
            errs() << "--> Function: " << demangle(C.F->getName()) << "\n";
            errs() << "--> Contract: " << C.ContractString << "\n";
            errs() << "\n";
        }
    }

    return PreservedAnalyses::all();
}

/*
 * Apply worklist algorithm
 * Need Start param to make sure that the initialization of parameters does not count as write operation
 * Start param should be the corresponding DbgDeclareInst. It is therefore also not counted.
 */
std::map<const Instruction*, std::set<ContractVerifierPass::RWStatus>> ContractVerifierPass::WorklistRW(Value* val, const Instruction* Start, const Function* F, bool must) {
    std::map<const Instruction*, std::set<RWStatus>> postAccess;
    std::vector<std::pair<const Instruction*, std::set<RWStatus>>> todoList = { {Start->getNextNonDebugInstruction(), std::set<RWStatus>() } };
    while (!todoList.empty()) {
        const Instruction* next = todoList.begin()->first;
        std::set<RWStatus> prevInfo = todoList.begin()->second;

        while (next != nullptr) {
            // Add previous info depending on following conditions:
            // 1. In any case, prevInfo MUST have the corresponding access
            // 2. Either: It is a "may" analysis, "next" was until now unreachable, or it was reached before and already contains the access
            // If those apply, add that access. Otherwise, remove it if present (=> "must" analysis, node reached with info, jumped here without)
            if (!postAccess.contains(next)) {
                if (prevInfo.contains(RWStatus::READ)) postAccess[next].insert(RWStatus::READ);
                if (prevInfo.contains(RWStatus::WRITE)) postAccess[next].insert(RWStatus::WRITE);
            } else {
                for (RWStatus s : { RWStatus::WRITE, RWStatus::READ } ) {
                    if (prevInfo.contains(s) && (!must || postAccess[next].contains(s)))
                        postAccess[next].insert(s);
                    else
                        postAccess[next].erase(s);
                }
            }

            // Check for read or write operations
            if (const LoadInst* Load = dyn_cast<LoadInst>(next)) {
                if (Load->getPointerOperand() == val) {
                    // Variable is being loaded here. Add READ to this instruction
                    postAccess[next].insert(RWStatus::READ);
                }
            }
            if (const StoreInst* Store = dyn_cast<StoreInst>(next)) {
                if (Store->getPointerOperand() == val) {
                    // Variable is being stored here. Add WRITE to this instruction
                    postAccess[next].insert(RWStatus::WRITE);
                }
            }

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

            if (getenv("LLVMCONTRACT_DEBUG") != NULL && atoi(getenv("LLVMCONTRACT_DEBUG")) == 1) {
                std::stringstream out;
                out << (postAccess[next].contains(RWStatus::READ) ? "R" : "") << (postAccess[next].contains(RWStatus::WRITE) ? "W" : "");
                LLVMContext& C = next->getContext();
                MDNode* N = MDNode::get(C, MDString::get(C, out.str()));
                ((Instruction*)next)->setMetadata(("llvmcontract.RWdebug." + val->getName()).str(), N);
            }

            // Update for next iteration
            prevInfo = postAccess[next];
            next = next->getNextNonDebugInstruction();
        }
        todoList.erase(todoList.begin());
    }
    return postAccess;
}

std::set<ContractVerifierPass::RWStatus> ContractVerifierPass::checkVarRW(std::string var, const Function* F, bool must, std::string& error) {
    Value* varValue = nullptr;
    const DbgDeclareInst* decl = nullptr;
    for (const BasicBlock& B : *F) {
        for (const Instruction& I : B) {
            // Look for declaration of variable to get mapping of Variable -> Value
            if (const DbgDeclareInst* DbgDec = dyn_cast<const DbgDeclareInst>(&I)) {
                DILocalVariable* Variable = DbgDec->getVariable();
                // Check variable name fits
                if (Variable->getName() != var) continue;

                // Check if scope fits
                if (Variable->getScope() != F->getSubprogram()) continue;

                varValue = DbgDec->getVariableLocationOp(0);
                decl = DbgDec;
                break;
            }
        }
        if (varValue != nullptr) break;
    }

    if (varValue == nullptr) {
        error = "Variable \"" + var + "\" does not exist!\n";
        return std::set<RWStatus>();
    }

    // For easier debugging
    varValue->setName(F->getName() + "." + var);

    std::map<const Instruction*, std::set<RWStatus>> AnalysisInfo = WorklistRW(varValue, decl, F, must);

    // Take intersection of all returning instructions
    std::set<RWStatus> final = { RWStatus::READ, RWStatus::WRITE };
    for (const BasicBlock& B : *F) {
        for (const Instruction& I : B) {
            if (const ReturnInst* RI = dyn_cast<ReturnInst>(&I)) {
                if (!AnalysisInfo[RI].contains(RWStatus::READ)) final.erase(RWStatus::READ);
                if (!AnalysisInfo[RI].contains(RWStatus::WRITE)) final.erase(RWStatus::WRITE);
            }
        }
    }
    return final;
}
