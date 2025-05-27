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
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;
using namespace ContractTree;

void outputSubformulaErrs(std::string type, const std::vector<std::shared_ptr<ContractFormula>> set, std::map<std::shared_ptr<ContractFormula>, std::string> reasons) {
    for (std::shared_ptr<ContractFormula> form : set) {
        if (*form->Status == Fulfillment::FULFILLED) continue;
        errs() << "--> " + type + " Subformula Status: " << FulfillmentStr(*form->Status) << "\n";
        if (*form->Status > Fulfillment::FULFILLED) {
            errs() << "    --> Formula String: " << form->ExprStr << "\n";
            if (reasons.contains(form))
                errs() << "    --> Message: " << reasons[form] << "\n";
            errs() << "    --> Error Info:\n";
            for (std::string errinfo : *form->ErrorInfo) {
                errs() << "        " << errinfo << "\n";
            }
        }
    }
}

Fulfillment ContractPostProcessingPass::checkExpressions(ContractManagerAnalysis::Contract const& C, bool output) {
    Fulfillment s = Fulfillment::FULFILLED;
    std::map<std::shared_ptr<ContractFormula>, std::string> reasons;
    for (const std::shared_ptr<ContractFormula> Expr : C.Data.Pre) {
        std::pair<Fulfillment,std::optional<std::string>> result = resolveFormula(Expr);
        if (result.second) reasons[Expr] = *result.second;
        s = std::max(s, result.first);
    }
    for (const std::shared_ptr<ContractFormula> Expr : C.Data.Post) {
        std::pair<Fulfillment,std::optional<std::string>> result = resolveFormula(Expr);
        if (result.second) reasons[Expr] = *result.second;
        s = std::max(s, result.first);
    }
    if (!output) return s;

    if (s == Fulfillment::FULFILLED && IS_DEBUG) WithColor(errs(), HighlightColor::String) << "## Contract Fulfilled! ##\n";
    if (s == Fulfillment::FULFILLED && !IS_DEBUG) return s; // No debug output, don't spam fulfilled contracts

    if (s == Fulfillment::UNKNOWN) WithColor(errs(), HighlightColor::Warning) << "## Contract Status Unknown ##\n";
    if (s == Fulfillment::BROKEN) WithColor(errs(), HighlightColor::Error) << "## Contract violation detected! ##\n";
    errs() << "--> Function: " << demangle(C.F->getName()) << "\n";
    errs() << "--> Contract: " << C.ContractString << "\n";
    if (s > Fulfillment::FULFILLED) {
        outputSubformulaErrs("Precondition", C.Data.Pre, reasons);
        outputSubformulaErrs("Postcondition", C.Data.Post, reasons);
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

    bool haveCorrContr = false;
    for (ContractManagerAnalysis::Contract C : DB.Contracts) {
        if (C.Data.xres != Fulfillment::UNKNOWN) {
            haveCorrContr = true;
            checkExpErr(C);
        }
    }
    if (haveCorrContr) {
        errs() << "Checking correctness contract results:\n";
        errs() << "\nTotal number of correctness contracts: " << xfail + xsucc << "\n";
        errs() << "Total number of xfail contracts: " << xfail << "\n";
        errs() << "Total number of xsucc contracts: " << xsucc << "\n";
        errs() << "Total number of FN: " << FN << "\n";
        errs() << "Total number of FP: " << FP << "\n\n";
        errs() << "Total number of UN: " << UN << "\n\n";
        errs() << "Checking verification contract results:\n";
    }

    for (ContractManagerAnalysis::Contract C : DB.Contracts) {
        if (C.Data.xres == Fulfillment::UNKNOWN) {
            checkExpressions(C, true);
        }
    }

    std::stringstream s;
    s << "CoVer: Total Tool Runtime " << std::fixed << std::chrono::duration<double>(std::chrono::system_clock::now() - DB.start_time).count() << "s\n";
    errs() << s.str();

    return PreservedAnalyses::all();
}

std::pair<Fulfillment,std::optional<std::string>> ContractPostProcessingPass::resolveFormula(std::shared_ptr<ContractFormula> contrF) {
    if (contrF->Children.empty()) {
        return {*contrF->Status, *contrF->Status == Fulfillment::FULFILLED ? std::nullopt : contrF->Message};
    }
    std::vector<Fulfillment> fs;
    std::optional<std::string> outStr = contrF->Message;
    for (std::shared_ptr<ContractFormula> Form : contrF->Children) {
        std::pair<Fulfillment,std::optional<std::string>> children = resolveFormula(Form);
        if (children.second) {
            if (!outStr) outStr = "";
            *outStr += ", " + *children.second;
        }
        fs.push_back(children.first);
    }
    switch (contrF->type) {
        case FormulaType::OR:
            *contrF->Status = *std::min_element(fs.begin(), fs.end());
            if (*contrF->Status != Fulfillment::FULFILLED) {
                // Add error info from children. As its an OR: All must not be fulfilled, so concat all
                contrF->ErrorInfo->push_back("No children satisfied for subformula: " + contrF->ExprStr);
                for (std::shared_ptr<ContractFormula> Form : contrF->Children) {
                    contrF->ErrorInfo->push_back("Error Info for child: " + Form->ExprStr);
                    contrF->ErrorInfo->insert(contrF->ErrorInfo->end(), Form->ErrorInfo->begin(), Form->ErrorInfo->end());
                }
            }
            return {*contrF->Status, outStr};
            break;
        case FormulaType::XOR:
            std::shared_ptr<ContractFormula> prevFulfil = nullptr;
            for (std::shared_ptr<ContractFormula> Form : contrF->Children) {
                if (!prevFulfil && *Form->Status == Fulfillment::FULFILLED) {
                    prevFulfil = Form; // First fulfillment
                    continue;
                }
                if (prevFulfil && *Form->Status == Fulfillment::FULFILLED) {
                    // At least two children fulfilled -> error
                    contrF->ErrorInfo->push_back("More than one child satisfied for subformula: " + contrF->ExprStr);
                    if (!prevFulfil->ErrorInfo->empty()) {
                        contrF->ErrorInfo->push_back("Messages from Child 1: " + prevFulfil->ExprStr);
                        contrF->ErrorInfo->insert(contrF->ErrorInfo->end(), prevFulfil->ErrorInfo->begin(), prevFulfil->ErrorInfo->end());
                    }
                    if (!Form->ErrorInfo->empty()) {
                        contrF->ErrorInfo->push_back("Messages from Child 2: " + Form->ExprStr);
                        contrF->ErrorInfo->insert(contrF->ErrorInfo->end(), Form->ErrorInfo->begin(), Form->ErrorInfo->end());
                    }
                    *contrF->Status = Fulfillment::BROKEN;
                    return {*contrF->Status, outStr};
                }
                if (*Form->Status == Fulfillment::UNKNOWN) {
                    contrF->ErrorInfo->push_back("Child fulfillment unknown for subformula: " + contrF->ExprStr);
                    contrF->ErrorInfo->push_back("And child: " + Form->ExprStr);
                    if (!Form->ErrorInfo->empty()) {
                        contrF->ErrorInfo->push_back("Messages from unknown fulfillment Child: ");
                        contrF->ErrorInfo->insert(contrF->ErrorInfo->end(), Form->ErrorInfo->begin(), Form->ErrorInfo->end());
                    }
                    *contrF->Status = Fulfillment::UNKNOWN;
                    return {*contrF->Status, outStr};
                }
            }
            if (prevFulfil) {
                // At least one success logged, everythin is fine
                contrF->ErrorInfo->push_back("Exactly one child satisfied of subformula: " + contrF->ExprStr);
                *contrF->Status = Fulfillment::FULFILLED;
                return {*contrF->Status, std::nullopt};
            }
            // Not success or unknown, so no child satisfied
            contrF->ErrorInfo->push_back("No child satisfied for subformula: " + contrF->ExprStr);
            for (std::shared_ptr<ContractFormula> Form : contrF->Children) {
                if (!Form->ErrorInfo->empty()) {
                    contrF->ErrorInfo->push_back("Messages from Child: " + Form->ExprStr);
                    contrF->ErrorInfo->insert(contrF->ErrorInfo->end(), Form->ErrorInfo->begin(), Form->ErrorInfo->end());
                }
            }
            *contrF->Status = Fulfillment::BROKEN;
            return {*contrF->Status, outStr};
    }
    llvm_unreachable("Unknown composite in contract definition!");
}
