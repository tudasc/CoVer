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
        ContractLangSyntaxError(size_t _line, size_t _charpos) : linePosition(_line), charPositionInLine(_charpos) {}
        const char * what() const noexcept override {
            return "Syntax error occured while parsing contract";
        }
        size_t linePos() { return linePosition; }
        size_t charPos() { return charPositionInLine; }
    private:
        antlr4::Token *offendingSymbol;
        size_t linePosition;
        size_t charPositionInLine;
};
