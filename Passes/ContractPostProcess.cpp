#include "ContractPostProcess.hpp"
#include "ContractManager.hpp"
#include "ContractTree.hpp"
#include <algorithm>
#include <json/value.h>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <llvm/Support/ErrorHandling.h>

import LLVMModule;
import ContractPassUtility;
import TUIManager;
import ErrorMessage;

using namespace llvm;
using namespace ContractTree;
using Contract = ContractManagerAnalysis::Contract;

static cl::opt<std::string> ClPrintJsonReports(
    "cover-generate-json-report", cl::init(""),
    cl::desc("Generate JSON report to specified file"),
    cl::Hidden);

std::error_code err_ostream;
raw_fd_ostream nullstream = raw_fd_ostream("/dev/null", err_ostream);
raw_ostream& ContractPostProcessingPass::printMsg() {
    if (isInteractive) return nullstream;
    return errs();
}

void ContractPostProcessingPass::outputSubformulaErrs(std::string type, const Contract C, std::map<std::shared_ptr<ContractFormula>, ErrorMessage> const& reasons) {
    std::shared_ptr<ContractFormula> scopeForm = type == "Precondition" ? C.Data.Pre : C.Data.Post;
    if (!scopeForm) return;
    for (std::shared_ptr<ContractFormula> form : scopeForm->Children) {
        if (*form->Status == Fulfillment::FULFILLED) continue;
        printMsg() << "--> " << type << " Subformula Status: " << FulfillmentStr(*form->Status) << "\n";
        if (*form->Status > Fulfillment::FULFILLED) {
            printMsg() << "    --> Formula String: " << form->ExprStr << "\n";
            if (reasons.contains(form))
                printMsg() << "    --> Message: " << reasons.at(form).text << "\n";
            printMsg() << "    --> Error Info:\n";
            for (ErrorMessage errinfo : DB->reports[form.get()]) {
                printMsg() << "        " << errinfo.text << "\n";
                Json::Value j;
                j["type"] = reasons.at(form).text;
                j["error_id"] = errinfo.error_id;
                j["text"] = errinfo.text;
                j["references"] = Json::arrayValue;
                for (FileReference const& ref : errinfo.references) {
                    Json::Value json_ref;
                    json_ref["file"] = ref.file;
                    json_ref["line"] = ref.line;
                    json_ref["column"] = ref.column;
                    j["references"].append(json_ref);
                }
                json_messages["messages"].append(j);
            }
        }
    }
}

Fulfillment ContractPostProcessingPass::checkExpressions(ContractManagerAnalysis::Contract const& C, bool output) {
    Fulfillment s = Fulfillment::FULFILLED;
    std::map<std::shared_ptr<ContractFormula>, ErrorMessage> reasons;
    if (C.Data.Pre) {
        std::pair<Fulfillment,std::optional<std::string>> result = resolveFormula(C.Data.Pre, reasons);
        s = std::max(s, result.first);
    }
    if (C.Data.Post) {
        std::pair<Fulfillment,std::optional<std::string>> result = resolveFormula(C.Data.Post, reasons);
        s = std::max(s, result.first);
    }
    if (!output) return s;

    if (s == Fulfillment::FULFILLED && ContractPassUtility::isDebug()) WithColor(printMsg(), HighlightColor::String) << "## Contract Fulfilled! ##\n";
    if (s == Fulfillment::FULFILLED && !ContractPassUtility::isDebug()) return s; // No debug output, don't spam fulfilled contracts

    if (s == Fulfillment::UNKNOWN) WithColor(printMsg(), HighlightColor::Warning) << "## Contract Status Unknown ##\n";
    if (s == Fulfillment::BROKEN) WithColor(printMsg(), HighlightColor::Error) << "## Contract violation detected! ##\n";
    printMsg() << "--> Function: " << demangle(C.F->getName()) << "\n";
    printMsg() << "--> Contract: " << C.ContractString << "\n";

    if (s > Fulfillment::FULFILLED) {
        ViolatedContracts.push_back(C);
        outputSubformulaErrs("Precondition", C, reasons);
        outputSubformulaErrs("Postcondition", C, reasons);
    }
    if (ContractPassUtility::isDebug()) {
        WithColor(printMsg(), HighlightColor::Remark) << "--> Debug Begin\n";
        for (std::string dbg: *C.DebugInfo) {
            WithColor(printMsg(), HighlightColor::Remark) << dbg << "\n";
        }
        WithColor(printMsg(), HighlightColor::Remark) << "<-- Debug End\n";
    }
    printMsg() << "\n";
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

    WithColor(printMsg(), col) << "Encountered " << err << " in Function \"" << demangle(C.F->getName()) << "\"\n";
    printMsg() << "Contract: " << C.ContractString << "\n";
}

PreservedAnalyses ContractPostProcessingPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    DB = &AM.getResult<ContractManagerAnalysis>(M);
    isInteractive = DB->isInteractive;

    json_messages["messages"] = {};

    bool haveCorrContr = false;
    for (ContractManagerAnalysis::Contract C : DB->Contracts) {
        if (C.Data.xres != Fulfillment::UNKNOWN) {
            haveCorrContr = true;
            checkExpErr(C);
        }
    }
    if (haveCorrContr) {
        printMsg() << "Checking correctness contract results:\n";
        printMsg() << "\nTotal number of correctness contracts: " << xfail + xsucc << "\n";
        printMsg() << "Total number of xfail contracts: " << xfail << "\n";
        printMsg() << "Total number of xsucc contracts: " << xsucc << "\n";
        printMsg() << "Total number of FN: " << FN << "\n";
        printMsg() << "Total number of FP: " << FP << "\n\n";
        printMsg() << "Total number of UN: " << UN << "\n\n";
        printMsg() << "Checking verification contract results:\n";
    }

    for (ContractManagerAnalysis::Contract C : DB->Contracts) {
        if (C.Data.xres == Fulfillment::UNKNOWN) {
            checkExpressions(C, true);
        }
    }

    std::stringstream s;
    s << "CoVer: Total Tool Runtime " << std::fixed << std::chrono::duration<double>(std::chrono::system_clock::now() - DB->start_time).count() << "s\n\n";
    printMsg() << s.str();

    if (isInteractive) {
        bool reanalyse = TUIManager::ResultsScreen(ViolatedContracts, DB->reports);
        if (!reanalyse) {
            std::filesystem::remove("CoVer_InteractStart.ll");
            std::filesystem::remove("CoVer_Reanalyse.ll");
            std::filesystem::remove("CoVer_Reanalyse.ll.orig");
            std::filesystem::remove("CoVer_HistoryCache");
        }
        else exit(0);
    }

    // Write json to file and database
    DB->processedReports = json_messages;
    if (!ClPrintJsonReports.empty()) {
        std::ofstream file(ClPrintJsonReports);
        file << json_writer.write(json_messages);
        file.close();
    }

    return PreservedAnalyses::all();
}

std::pair<Fulfillment,std::optional<std::string>> ContractPostProcessingPass::resolveFormula(std::shared_ptr<ContractFormula> contrF, std::map<std::shared_ptr<ContractFormula>,ErrorMessage>& reasons) {
    std::optional<std::string> outMsg = contrF->Message;
    if (contrF->Message) reasons[contrF] = {.text = *outMsg};
    if (contrF->Children.empty()) {
        return {*contrF->Status, *contrF->Status == Fulfillment::FULFILLED ? std::nullopt : contrF->Message};
    }
    std::vector<Fulfillment> fs;
    for (std::shared_ptr<ContractFormula> Form : contrF->Children) {
        std::pair<Fulfillment,std::optional<std::string>> children = resolveFormula(Form, reasons);
        fs.push_back(children.first);
    }
    switch (contrF->type) {
        case FormulaType::AND:
        case FormulaType::OR:
            if (contrF->type == FormulaType::AND) *contrF->Status = *std::max_element(fs.begin(), fs.end());
            else *contrF->Status = *std::min_element(fs.begin(), fs.end());
            if (*contrF->Status != Fulfillment::FULFILLED) {
                // Add error info from unfulfilled children
                DB->reports[contrF.get()].push_back({ .text = (contrF->type == FormulaType::OR ? "No children" : "At least one child not") + (" satisfied for subformula: " + contrF->ExprStr)});
                for (std::shared_ptr<ContractFormula> Form : contrF->Children) {
                    if (*Form->Status == Fulfillment::FULFILLED) continue;
                    DB->reports[contrF.get()].push_back({.text = "Error Info for child: " + Form->ExprStr});
                    DB->reports[contrF.get()].insert(DB->reports[contrF.get()].end(), DB->reports[Form.get()].begin(), DB->reports[Form.get()].end());
                }
            }
            return {*contrF->Status, outMsg};
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
                    DB->reports[contrF.get()].push_back({.text = "More than one child satisfied for subformula: " + contrF->ExprStr});
                    if (!DB->reports[prevFulfil.get()].empty()) {
                        DB->reports[contrF.get()].push_back({.text = "Messages from Child 1: " + prevFulfil->ExprStr});
                        DB->reports[contrF.get()].insert(DB->reports[contrF.get()].end(), DB->reports[prevFulfil.get()].begin(), DB->reports[prevFulfil.get()].end());
                    }
                    if (!DB->reports[Form.get()].empty()) {
                        DB->reports[contrF.get()].push_back({.text = "Messages from Child 2: " + Form->ExprStr});
                        DB->reports[contrF.get()].insert(DB->reports[contrF.get()].end(), DB->reports[Form.get()].begin(), DB->reports[Form.get()].end());
                    }
                    *contrF->Status = Fulfillment::BROKEN;
                    return {*contrF->Status, outMsg};
                }
                if (*Form->Status == Fulfillment::UNKNOWN) {
                    DB->reports[contrF.get()].push_back({.text = "Child fulfillment unknown for subformula: " + contrF->ExprStr});
                    DB->reports[contrF.get()].push_back({.text = "And child: " + Form->ExprStr});
                    if (!DB->reports[Form.get()].empty()) {
                        DB->reports[contrF.get()].push_back({.text = "Messages from unknown fulfillment Child: "});
                        DB->reports[contrF.get()].insert(DB->reports[contrF.get()].end(), DB->reports[Form.get()].begin(), DB->reports[Form.get()].end());
                    }
                    *contrF->Status = Fulfillment::UNKNOWN;
                    return {*contrF->Status, outMsg};
                }
            }
            if (prevFulfil) {
                // At least one success logged, everything is fine
                DB->reports[contrF.get()].push_back({.text = "Exactly one child satisfied of subformula: " + contrF->ExprStr});
                *contrF->Status = Fulfillment::FULFILLED;
                return {*contrF->Status, std::nullopt};
            }
            // Not success or unknown, so no child satisfied
            DB->reports[contrF.get()].push_back({.text = "No child satisfied for subformula: " + contrF->ExprStr});
            for (std::shared_ptr<ContractFormula> Form : contrF->Children) {
                if (!DB->reports[Form.get()].empty()) {
                    DB->reports[contrF.get()].push_back({.text = "Messages from Child: " + Form->ExprStr});
                    DB->reports[contrF.get()].insert(DB->reports[contrF.get()].end(), DB->reports[Form.get()].begin(), DB->reports[Form.get()].end());
                }
            }
            *contrF->Status = Fulfillment::BROKEN;
            return {*contrF->Status, outMsg};
    }
    llvm_unreachable("Unknown composite in contract definition!");
}
