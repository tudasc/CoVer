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
#include <memory>
#include <optional>
#include <string>

#include <antlr4-runtime.h>
#include <vector>
#include "ContractLexer.h"
#include "ContractParser.h"
#include "../LangCode/ContractLangErrorListener.hpp"
#include "../LangCode/ContractDataVisitor.hpp"
#include "ContractTree.hpp"

#include "ContractPassUtility.hpp"

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

AnalysisKey ContractManagerAnalysis::Key;

ContractManagerAnalysis::ContractDatabase ContractManagerAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
    ContractDatabase curDatabase;
    curDatabase.start_time = std::chrono::system_clock::now();

    errs() << "Running Contract Manager on Module: " << M.getName() << "\n";

    GlobalVariable* Annotations = M.getGlobalVariable("llvm.global.annotations");
    if (Annotations == nullptr) {
        errs() << "No annotations present, quitting." << M.getName() << "\n";
        return curDatabase;
    }

    Constant* ANNValues = Annotations->getInitializer();

    ContractLangErrorListener listener;
    ContractDataVisitor dataVisitor;

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
            errs() << "Detected non-contract annotation (Lexing Error at " << e.linePos() << ":" << e.charPos() << "), ignoring: " << ANNStr << "\n";
            continue;
        }

        // Apply Parser.
        ContractParser parser(&tokens);
        parser.removeErrorListeners();
        parser.addErrorListener(&listener);
        try {
            parser.contract();
        } catch (ContractLangSyntaxError& e) {
            errs() << "Detected non-contract annotation (Parser Error at " << e.linePos() << ":" << e.charPos() << "), ignoring: " << ANNStr << "\n";
            if (IS_DEBUG) {
                for (auto x : tokens.getTokens()) {
                    errs() << "(" << x->getText() << "," << lexer.getVocabulary().getSymbolicName(x->getType()) << ")\n";
                }
            }
            continue;
        }
        Function* F =  dyn_cast<Function>(ANN->getOperand(0));
        StringRef location = dyn_cast<ConstantDataArray>(dyn_cast<GlobalVariable>(ANN->getOperand(2))->getInitializer())->getAsCString();
        errs() << "Found contract in " << location << " with content: " << ANNStr << "\n";
        parser.reset();

        // Finally have contract data
        ContractData Data = dataVisitor.getContractData(parser.contract());
        Contract newCtr{F, ANNStr, Data};

        // Add normal contract
        curDatabase.Contracts.push_back(newCtr);

        // Create and add Linearized Contract
        std::vector<std::shared_ptr<ContractExpression>> PreLin;
        for (const std::shared_ptr<ContractFormula> contrF : newCtr.Data.Pre) {
            std::vector<std::shared_ptr<ContractExpression>> contrFLin = linearizeContractFormula(contrF);
            PreLin.insert( PreLin.end(), contrFLin.begin(), contrFLin.end() );
        }
        std::vector<std::shared_ptr<ContractExpression>> PostLin;
        for (const std::shared_ptr<ContractFormula> contrF : newCtr.Data.Post) {
            std::vector<std::shared_ptr<ContractExpression>> contrFLin = linearizeContractFormula(contrF);
            PostLin.insert( PostLin.end(), contrFLin.begin(), contrFLin.end() );
        }

        LinearizedContract lContract = { F, ANNStr, PreLin, PostLin, newCtr.DebugInfo};
        curDatabase.LinearizedContracts.push_back(lContract);

        // Append tag database
        curDatabase.Tags[newCtr.F].insert(curDatabase.Tags[newCtr.F].end(), newCtr.Data.Tags.begin(), newCtr.Data.Tags.end());
    }

    std::stringstream s;
    s << "LLVMContractsPlugin: Parsed contracts after " << std::fixed << std::chrono::duration<double>(std::chrono::system_clock::now() - curDatabase.start_time).count() << "s\n";
    errs() << s.str();

    return curDatabase;
}

const std::vector<std::shared_ptr<ContractExpression>> ContractManagerAnalysis::linearizeContractFormula(const std::shared_ptr<ContractFormula> contrF) {
    if (contrF->Children.empty()) {
        assert(std::dynamic_pointer_cast<ContractExpression>(contrF) != nullptr);
        return { std::dynamic_pointer_cast<ContractExpression>(contrF) };
    }
    std::vector<std::shared_ptr<ContractExpression>> exprs;
    for (std::shared_ptr<ContractFormula> form : contrF->Children ) {
        std::vector<std::shared_ptr<ContractExpression>> linChild = linearizeContractFormula(form);
        exprs.insert( exprs.end(), linChild.begin(), linChild.end() );
    }
    return exprs;
}
