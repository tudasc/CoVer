#include "ContractVerifierRW.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include "ContractPassUtility.hpp"

#include <llvm/Analysis/InlineCost.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Instrumentation.h>
#include <set>

using namespace llvm;
using namespace ContractTree;

PreservedAnalyses ContractVerifierRWPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    ContractManagerAnalysis::ContractDatabase DB = AM.getResult<ContractManagerAnalysis>(M);

    for (ContractManagerAnalysis::Contract C : DB.Contracts) {
        if (*C.Status == Fulfillment::UNKNOWN && !C.Data.Pre.has_value() && C.Data.Post.has_value()) {
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
                default: continue;
            }
            if (!err.empty()) {
                errs() << err << "\n";
            }
            if (result) {
                *C.Status = Fulfillment::FULFILLED;
            } else {
                *C.Status = Fulfillment::BROKEN;
            }
        }
    }

    return PreservedAnalyses::all();
}

std::set<ContractVerifierRWPass::RWStatus> transferRW(std::set<ContractVerifierRWPass::RWStatus> cur, const Instruction* I, void* data) {
    Value* val = static_cast<Value*>(data);
    // Check for read or write operations
    if (const LoadInst* Load = dyn_cast<LoadInst>(I)) {
        if (Load->getPointerOperand() == val) {
            // Variable is being loaded here. Add READ to this instruction
            cur.insert(ContractVerifierRWPass::RWStatus::READ);
        }
    }
    if (const StoreInst* Store = dyn_cast<StoreInst>(I)) {
        if (Store->getPointerOperand() == val) {
            // Variable is being stored here. Add WRITE to this instruction
            cur.insert(ContractVerifierRWPass::RWStatus::WRITE);
        }
    }
    return cur;
}

std::set<ContractVerifierRWPass::RWStatus> mergeRW(std::set<ContractVerifierRWPass::RWStatus> prev, std::set<ContractVerifierRWPass::RWStatus> cur, const Instruction* I, void* data) {
    for (ContractVerifierRWPass::RWStatus s : { ContractVerifierRWPass::RWStatus::WRITE, ContractVerifierRWPass::RWStatus::READ } ) {
        if (prev.contains(s) && cur.contains(s))
            cur.insert(s);
        else
            cur.erase(s);
    }
    return cur;
}

std::set<ContractVerifierRWPass::RWStatus> ContractVerifierRWPass::checkVarRW(std::string var, const Function* F, bool must, std::string& error) {
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

    std::map<const Instruction*, std::set<RWStatus>> AnalysisInfo = GenericWorklist<std::set<RWStatus>>(decl, transferRW, mergeRW, varValue, std::set<RWStatus>());

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
