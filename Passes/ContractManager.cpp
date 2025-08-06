#include "ContractManager.hpp"

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
#include <llvm/Support/CommandLine.h>
#include <memory>
#include <optional>
#include <string>

#include <vector>
#include "../LangCode/ContractDataExtractor.hpp"
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

ContractManagerAnalysis::ContractDatabase ContractManagerAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
    curDatabase.start_time = std::chrono::system_clock::now();

    errs() << "Running Contract Manager on Module: " << M.getName() << "\n";

    extractFromAnnotations(M);

    std::stringstream s;
    s << "CoVer: Parsed contracts after " << std::fixed << std::chrono::duration<double>(std::chrono::system_clock::now() - curDatabase.start_time).count() << "s\n";
    errs() << s.str();

    return curDatabase;
}

void ContractManagerAnalysis::extractFromAnnotations(const Module& M) {
    GlobalVariable* Annotations = M.getGlobalVariable("llvm.global.annotations");
    if (Annotations == nullptr) {
        errs() << "Note: No string annotations found.\n";
        return;
    }

    Constant* ANNValues = Annotations->getInitializer();

    for (Use& annUse : ANNValues->operands()) {
        ConstantStruct *ANN = dyn_cast<ConstantStruct>(annUse);
        StringRef ANNStr = dyn_cast<ConstantDataArray>(dyn_cast<GlobalVariable>(ANN->getOperand(1))->getInitializer())->getAsCString();
        Function* F =  dyn_cast<Function>(ANN->getOperand(0));
        addContract(ANNStr, F);
    }
}

void ContractManagerAnalysis::addContract(StringRef contract, Function* F) {

    std::optional<ContractData> Data = getContractData(contract.str());
    if (!Data) return;
    if (IS_DEBUG) errs() << "Found contract for function " << F->getName() << " with content: " << contract << "\n";

    // Finally have contract data
    Contract newCtr{F, contract, *Data};

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

    LinearizedContract lContract = { F, contract, PreLin, PostLin, newCtr.DebugInfo};
    curDatabase.LinearizedContracts.push_back(lContract);

    // Append tag database
    curDatabase.Tags[newCtr.F].insert(curDatabase.Tags[newCtr.F].end(), newCtr.Data.Tags.begin(), newCtr.Data.Tags.end());
}

const std::vector<std::shared_ptr<ContractExpression>> ContractManagerAnalysis::linearizeContractFormula(const std::shared_ptr<ContractFormula> contrF) {
    if (contrF->Children.empty()) {
        return { std::static_pointer_cast<ContractExpression>(contrF) };
    }
    std::vector<std::shared_ptr<ContractExpression>> exprs;
    for (std::shared_ptr<ContractFormula> form : contrF->Children ) {
        std::vector<std::shared_ptr<ContractExpression>> linChild = linearizeContractFormula(form);
        exprs.insert( exprs.end(), linChild.begin(), linChild.end() );
    }
    return exprs;
}
