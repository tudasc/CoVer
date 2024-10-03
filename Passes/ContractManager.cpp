#include "ContractManager.hpp"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/IPO/Attributor.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>
#include <optional>
#include <string>

#include <antlr4-runtime.h>
#include "ContractLexer.h"
#include "ContractParser.h"

using namespace llvm;

static std::optional<std::string> getFuncName(CallBase* FuncCall) {
    if (!FuncCall->getCalledFunction()) {
        if (!FuncCall->getCalledOperand()->getName().empty()) {
            return FuncCall->getCalledOperand()->getName().data();
        } else {
            return std::nullopt;
        }
    }
    return FuncCall->getCalledFunction()->getName().data();
}

PreservedAnalyses ContractManagerPass::run(Module &M, ModuleAnalysisManager &AM) {
    errs() << "Running Contract Manager on Module: " << M.getName() << "\n";

    GlobalVariable* Annotations = M.getGlobalVariable("llvm.global.annotations");
    if (Annotations == nullptr) errs() << "No annotations present, quitting." << M.getName() << "\n";

    Constant* ANNValues = Annotations->getInitializer();

    for (Use& annUse : ANNValues->operands()) {
        ConstantStruct *ANN = dyn_cast<ConstantStruct>(annUse);
        StringRef ANNStr = dyn_cast<ConstantDataArray>(dyn_cast<GlobalVariable>(ANN->getOperand(1))->getInitializer())->getAsCString();
        antlr4::ANTLRInputStream input(ANNStr);

        // Apply Lexer.
        ContractLexer lexer(&input);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&listener);
        antlr4::CommonTokenStream tokens(&lexer);
        try {
            tokens.fill();
        } catch (ContractLangSyntaxError& e) {
            errs() << "Detected non-contract annotation, ignoring: " << ANNStr << "\n";
            continue;
        }

        // Apply Parser.
        ContractParser parser(&tokens);
        parser.removeErrorListeners();
        parser.addErrorListener(&listener);
        #warning TODO parser error handling
        Function* F =  dyn_cast<Function>(ANN->getOperand(0));
        StringRef location = dyn_cast<ConstantDataArray>(dyn_cast<GlobalVariable>(ANN->getOperand(2))->getInitializer())->getAsCString();
        errs() << "Found contract in " << location << " with content: " << ANNStr << "\n";
    }

    return PreservedAnalyses::all();
}
