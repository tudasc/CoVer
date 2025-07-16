#include <optional>

#include "ContractDataVisitor.hpp"
#include "ContractLangErrorListener.hpp"
#include "ContractLexer.h"
#include "ContractParser.h"
#include "ContractTree.hpp"

using namespace ContractTree;

std::optional<ContractData> getContractData(std::string contract) {
    ContractDataVisitor dataVisitor;
    ContractLangErrorListener listener;
    antlr4::ANTLRInputStream input(contract);
    ContractLexer lexer(&input);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&listener);
    antlr4::CommonTokenStream tokens(&lexer);
    try {
        tokens.fill();
    } catch (ContractLangSyntaxError& e) {
        std::cerr << "Detected non-contract annotation (Lexing Error at " << e.linePos() << ":" << e.charPos() << "), ignoring: " << contract << "\n";
        return std::nullopt;
    }

    // Apply Parser.
    ContractParser parser(&tokens);
    parser.removeErrorListeners();
    parser.addErrorListener(&listener);
    try {
        return std::any_cast<ContractData>(dataVisitor.visit(parser.contract()));
    } catch (ContractLangSyntaxError& e) {
        std::cerr << "Detected non-contract annotation (Parser Error at " << e.linePos() << ":" << e.charPos() << "), ignoring: " << contract << "\n";
        return std::nullopt;
    }
}
