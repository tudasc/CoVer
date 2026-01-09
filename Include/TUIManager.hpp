#pragma once

#include <cassert>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <json/value.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <vector>

#include "../Passes/ContractManager.hpp"

constexpr int UI_ANALYSISINFO_PAD_SIZE = 15;

using namespace llvm;

using Contract = llvm::ContractManagerAnalysis::Contract;

namespace TUIManager {
    void StartMenu(ContractManagerAnalysis::ContractDatabase DB);
    bool ShowContractDetails(ContractManagerAnalysis::Contract C);
    bool ResultsScreen(std::vector<Contract> const& ViolatedContracts);

    std::string RenderTxtEntry(std::vector<ftxui::Element> lines, std::string title, std::string last_res);
    int RenderMenu(std::vector<std::string> choices, std::string title);
    void ShowFile(std::string file, std::map<int,ftxui::Color> highlights, int focus_line = -1);
    void ShowLines(std::vector<ftxui::Element> lines, std::string title, int focus = 0);

    std::string getSpecificLine(std::string file, int line);
}
