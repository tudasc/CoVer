#include "ContractPostProcess.hpp"
#include "ContractManager.hpp"
#include "ContractPassUtility.hpp"
#include "ContractTree.hpp"
#include <algorithm>
#include <cstddef>
#include <llvm/Demangle/Demangle.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/WithColor.h>
#include <memory>
#include <vector>

using namespace llvm;
using namespace ContractTree;

Fulfillment ContractPostProcessingPass::checkExpressions(ContractManagerAnalysis::Contract const& C, bool output) {
    Fulfillment s = Fulfillment::FULFILLED;
    std::string reason;
    for (const std::shared_ptr<ContractFormula> Expr : C.Data.Pre) {
        s = std::max(s, resolveFormula(Expr));
    }
    for (const std::shared_ptr<ContractFormula> Expr : C.Data.Post) {
        s = std::max(s, resolveFormula(Expr));
    }
    if (!output) return s;

    if (s == Fulfillment::FULFILLED) WithColor(errs(), HighlightColor::String) << "## Contract Fulfilled! ##\n";
    if (s == Fulfillment::UNKNOWN) WithColor(errs(), HighlightColor::Warning) << "## Contract Status Unknown ##\n";
    if (s == Fulfillment::BROKEN) WithColor(errs(), HighlightColor::Error) << "## Contract violation detected! ##\n";
    errs() << "--> Function: " << demangle(C.F->getName()) << "\n";
    errs() << "--> Contract: " << C.ContractString << "\n";
    if (s > Fulfillment::FULFILLED) {
        for (size_t i = 0; i < C.Data.Pre.size(); i++) {
            errs() << "--> Precondition Status, Index " << i << ": " << FulfillmentStr(*C.Data.Pre[i]->Status) << "\n";
            if (*C.Data.Pre[i]->Status > Fulfillment::FULFILLED)
                errs() << "    --> Expression String: " << C.Data.Pre[i]->ExprStr << "\n";
        }
        for (size_t i = 0; i < C.Data.Post.size(); i++) {
            errs() << "--> Postcondition Status, Index " << i << ": " << FulfillmentStr(*C.Data.Post[i]->Status) << "\n";
            if (*C.Data.Post[i]->Status > Fulfillment::FULFILLED)
                errs() << "    --> Expression String: " << C.Data.Post[i]->ExprStr << "\n";
        }
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
            checkExpressions(C, true);
        }
    }

    return PreservedAnalyses::all();
}

Fulfillment ContractPostProcessingPass::resolveFormula(std::shared_ptr<ContractFormula> contrF) {
    if (contrF->Children.empty()) {
        return *contrF->Status;
    }
    std::vector<Fulfillment> fs;
    for (std::shared_ptr<ContractFormula> Form : contrF->Children) {
        fs.push_back(resolveFormula(Form));
    }
    switch (contrF->type) {
        case FormulaType::OR:
            *contrF->Status = *std::min_element(fs.begin(), fs.end());
            return *contrF->Status;
            break;
        case FormulaType::XOR:
            bool hasBeenFulfilled = false;
            for (Fulfillment f : fs) {
                if (f == Fulfillment::UNKNOWN) {
                    // Unknown overrides everything
                    *contrF->Status = Fulfillment::UNKNOWN;
                    return *contrF->Status;
                } else if (!hasBeenFulfilled && f == Fulfillment::FULFILLED) {
                    // Not been fulfilled, XOR might hold
                    hasBeenFulfilled = true;
                } else if (hasBeenFulfilled && f == Fulfillment::FULFILLED) {
                    // XOR does not hold
                    *contrF->Status = Fulfillment::BROKEN;
                    break;
                }
            }
            if (hasBeenFulfilled && *contrF->Status != Fulfillment::BROKEN) {
                // Exactly one fulfillment, XOR holds
                *contrF->Status = Fulfillment::FULFILLED;
                return *contrF->Status;
            } else {
                // Not unknown, otherwise early return
                // Not success, otherwise above holds
                // Therefore: Error condition
                *contrF->Status = Fulfillment::BROKEN;
                return *contrF->Status;
            }
    }
    llvm_unreachable("Unknown composite in contract definition!");
}
