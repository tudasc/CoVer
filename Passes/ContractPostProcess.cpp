#include "ContractPostProcess.hpp"
#include "ContractManager.hpp"
#include "ContractPassUtility.hpp"
#include "ContractTree.hpp"
#include <algorithm>
#include <llvm/Demangle/Demangle.h>
#include <llvm/Support/WithColor.h>

using namespace llvm;
using namespace ContractTree;

Fulfillment ContractPostProcessingPass::checkExpressions(ContractManagerAnalysis::Contract const& C, bool output) {
    Fulfillment s = Fulfillment::FULFILLED;
    std::string reason;
    if (C.Data.Pre.has_value()) {
        s = std::max(s, *C.Data.Pre->Status);
    }
    if (C.Data.Post.has_value()) {
        s = std::max(s, *C.Data.Post->Status);
    }
    if (!output) return s;

    if (s == Fulfillment::FULFILLED) WithColor(errs(), HighlightColor::String) << "## Contract Fulfilled! ##\n";
    if (s == Fulfillment::UNKNOWN) WithColor(errs(), HighlightColor::Warning) << "## Contract Status Unknown ##\n";
    if (s == Fulfillment::BROKEN) WithColor(errs(), HighlightColor::Error) << "## Contract violation detected! ##\n";
    errs() << "--> Function: " << demangle(C.F->getName()) << "\n";
    errs() << "--> Contract: " << C.ContractString << "\n";
    if (s > Fulfillment::FULFILLED) {
        errs() << "--> Precondition Status: " << FulfillmentStr(*C.Data.Pre->Status) << "\n";
        errs() << "--> Postcondition Status: " << FulfillmentStr(*C.Data.Post->Status) << "\n";
    }
    if (IS_DEBUG) {
        WithColor(errs(), HighlightColor::Remark) << "--> Debug Begin\n";
        for (std::string dbg: *C.DebugInfo) {
            WithColor(errs(), HighlightColor::Remark) << dbg << "\n";
        }
        WithColor(errs(), HighlightColor::Remark) << "<-- Debug End\n";
    }
    errs() << "\n";
    return s;
}

void ContractPostProcessingPass::checkExpErr(ContractManagerAnalysis::Contract C) {
    HighlightColor col;
    std::string err;
    Fulfillment s = checkExpressions(C, false);
    if (s == Fulfillment::UNKNOWN) {
        col = HighlightColor::Warning;
        err = "UN";
    } else {
        col = HighlightColor::Error;
        if (C.Data.xres == Fulfillment::FULFILLED) {
            xsucc++;
            if (C.Data.xres == s) return; // No error
            FP++;
            err = "FP";
        } else {
            xfail++;
            if (C.Data.xres == s) return; // No error
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
    errs() << "Total number of xsucc contracts: " << xsucc << "\n";
    errs() << "Total number of FN: " << FN << "\n";
    errs() << "Total number of FP: " << FP << "\n\n";
    errs() << "Total number of UN: " << UN << "\n\n";

    errs() << "Checking verification contract results:\n";
    for (ContractManagerAnalysis::Contract C : DB.Contracts) {
        if (C.Data.xres == Fulfillment::UNKNOWN) {
            // Ignore contracts that only supply tags / Do nothing
            if (!C.Data.Pre.has_value() && !C.Data.Post.has_value()) continue;
            checkExpressions(C, true);
        }
    }

    return PreservedAnalyses::all();
}
