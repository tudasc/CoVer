#include "ContractVerifier.hpp"
#include "ContractManager.hpp"

#include <llvm/Analysis/InlineCost.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Instrumentation.h>

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
                checkVarRW(rOP.Variable, C.F, true);
            }
        }
    }

    return PreservedAnalyses::all();
}

bool ContractVerifierPass::checkVarRW(std::string var, const Function* F, bool read) {
    const BasicBlock& Entry = F->getEntryBlock();
    for (const Instruction& I : Entry) {
        // Look for declaration of variable to get mapping of Variable -> Value
        if (const DbgDeclareInst* DbgDec = dyn_cast<const DbgDeclareInst>(&I)) {
            DILocalVariable* Variable = DbgDec->getVariable();
            // Check variable name fits
            if (Variable->getName() != var) continue;

            errs() << "Found variable!" << "\n";
        }
    }
    return true;
}
