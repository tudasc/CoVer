#pragma once

#include "antlr4-runtime.h"

class ContractLangErrorListener : public antlr4::BaseErrorListener {
public:
    virtual void syntaxError(antlr4::Recognizer *recognizer,
                           antlr4::Token *offendingSymbol, size_t line,
                           size_t charPositionInLine, const std::string &msg,
                           std::exception_ptr e) override;
};

class ContractLangSyntaxError : public std::exception {
    public:
        const char * what() const noexcept override {
            return "Syntax error occured while parsing contract";
        }
};
