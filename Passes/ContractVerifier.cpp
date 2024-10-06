#include "ContractVerifier.hpp"
#include "ContractManager.hpp"

#include <llvm/Analysis/InlineCost.h>
#include <llvm/IR/PassManager.h>
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
            if (Expr.OP.type() == "ReadOperation") {
                // Loop through, check if variable is read
            }
        }
    }

    return PreservedAnalyses::all();
}
