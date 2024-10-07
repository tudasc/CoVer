#include "ContractVerifier.hpp"
#include "ContractManager.hpp"

#include <algorithm>
#include <iterator>
#include <llvm/Analysis/InlineCost.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Instrumentation.h>
#include <set>
#include <vector>

#include "../LangCode/ContractDataVisitor.hpp"

using namespace llvm;

PreservedAnalyses ContractVerifierPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    ContractManagerAnalysis::ContractDatabase DB = AM.getResult<ContractManagerAnalysis>(M);

    for (ContractManagerAnalysis::Contract C : DB.Contracts) {
        if (!C.Data.Pre.has_value() && C.Data.Post.has_value()) {
            // No preconditions
            const ContractDataVisitor::ContractExpression& Expr = C.Data.Post.value();
            if (Expr.OP->type() == "ReadOperation") {
                const ContractDataVisitor::ReadOperation& rOP = dynamic_cast<const ContractDataVisitor::ReadOperation&>(*Expr.OP);
                // Loop through, check if variable is read
                std::string err;
                bool isRead = checkVarRW(rOP.Variable, C.F, true, err).contains(RWStatus::READ);
                if (!err.empty()) {
                    errs() << err;
                    continue;
                }
                if (isRead) {
                    errs() << "Contract Fulfilled!\n";
                    C.Status = ContractManagerAnalysis::FULFILLED;
                } else {
                    errs() << "Contract violation detected!\n";
                    C.Status = ContractManagerAnalysis::BROKEN;
                }
            }
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
    #warning HACK assume Instruction*
    #warning TODO branches, "LUB"
    const Instruction* next = Start->getNextNonDebugInstruction();
    std::set<RWStatus> prevInfo;
    while (next != nullptr) {
        postAccess[next];
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
        // Add previous info
        postAccess[next].insert(prevInfo.begin(), prevInfo.end());

        // Update for next iteration
        prevInfo = postAccess[next];
        next = next->getNextNonDebugInstruction();
    }
    return postAccess;
}

std::set<ContractVerifierPass::RWStatus> ContractVerifierPass::checkVarRW(std::string var, const Function* F, bool must, std::string& error) {
    const BasicBlock& Entry = F->getEntryBlock();
    Value* varValue = nullptr;
    const DbgDeclareInst* decl = nullptr;
    for (const Instruction& I : Entry) {
        // Look for declaration of variable to get mapping of Variable -> Value
        if (const DbgDeclareInst* DbgDec = dyn_cast<const DbgDeclareInst>(&I)) {
            DILocalVariable* Variable = DbgDec->getVariable();
            // Check variable name fits
            if (Variable->getName() != var) continue;

            // Check if scope fits
            if (Variable->getScope() != F->getSubprogram()) continue;

            errs() << "Found variable!" << "\n";
            varValue = DbgDec->getVariableLocationOp(0);
            decl = DbgDec;
            break;
        }
    }

    if (varValue == nullptr) {
        error = "Variable \"" + var + "\" does not exist!\n";
        return std::set<RWStatus>();
    }

    // For easier debugging
    varValue->setName(var + ":" + F->getName());

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
