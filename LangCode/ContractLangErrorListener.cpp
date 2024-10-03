#include "ContractLangErrorListener.hpp"

#include <cstdlib>
#include <ostream>

#include "antlr4-runtime.h"

void ContractLangErrorListener::syntaxError(antlr4::Recognizer *recognizer,
                                        antlr4::Token *offendingSymbol,
                                        size_t line, size_t charPositionInLine,
                                        const std::string &msg,
                                        std::exception_ptr e) {
    // Don't perform any output here. That's for the pass to do if necessary.
    throw ContractLangSyntaxError();
}
