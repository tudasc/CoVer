#include "ContractPostProcess.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include <llvm/Demangle/Demangle.h>
#include <llvm/Support/WithColor.h>

using namespace llvm;
using namespace ContractTree;

void ContractPostProcessingPass::checkExpErr(ContractManagerAnalysis::Contract C) {
    HighlightColor col;
    std::string err;
    if (*C.Status == Fulfillment::UNKNOWN) {
        col = HighlightColor::Warning;
        err = "UN";
    } else {
        col = HighlightColor::Error;
        if (C.Data.xres == Fulfillment::FULFILLED) {
            xsucc++;
            if (C.Data.xres == *C.Status) return; // No error
            FP++;
            err = "FP";
        } else {
            xfail++;
            if (C.Data.xres == *C.Status) return; // No error
            FN++;
            err = "FN";
        }
    }

    WithColor(errs(), col) << "Encountered " << err << " in Function \"" << demangle(C.F->getName()) << "\"\n";
    errs() << "Contract: " << C.ContractString << "\n";
}

PreservedAnalyses ContractPostProcessingPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    ContractManagerAnalysis::ContractDatabase DB = AM.getResult<ContractManagerAnalysis>(M);

    errs() << "Checking correctness contract results:\n";
    for (ContractManagerAnalysis::Contract C : DB.Contracts) {
        if (C.Data.xres != Fulfillment::UNKNOWN) {
            checkExpErr(C);
        }
    }
    errs() << "\nTotal number of correctness contracts: " << xfail + xsucc << "\n";
    errs() << "Total number of xfail contracts: " << xfail << "\n";
    errs() << "Total number of xsucc contracts: " << xfail << "\n";
    errs() << "Total number of FN: " << FN << "\n";
    errs() << "Total number of FP: " << FP << "\n\n";
    errs() << "Total number of UN: " << UN << "\n\n";

    errs() << "Checking verification contract results:\n";
    for (ContractManagerAnalysis::Contract C : DB.Contracts) {
        if (C.Data.xres == Fulfillment::UNKNOWN) {
            if (*C.Status == Fulfillment::FULFILLED) WithColor(errs(), HighlightColor::Remark) << "## Contract Fulfilled! ##\n";
            if (*C.Status == Fulfillment::UNKNOWN) WithColor(errs(), HighlightColor::Warning) << "## Contract Status Unknown ##\n";
            if (*C.Status == Fulfillment::BROKEN) WithColor(errs(), HighlightColor::Error) << "## Contract violation detected! ##\n";
            errs() << "--> Function: " << demangle(C.F->getName()) << "\n";
            errs() << "--> Contract: " << C.ContractString << "\n";
            errs() << "\n";
        }
    }



    return PreservedAnalyses::all();
}
