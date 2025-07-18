#pragma once

#include <antlr4-runtime.h>
#include <any>

#include "ContractParserBaseVisitor.h"

class ContractDataVisitor : public ContractParserBaseVisitor {
    public:
        std::any visitContract(ContractParser::ContractContext *ctx) override;
        std::any visitExprList(ContractParser::ExprListContext *ctx) override;
        std::any visitExprFormula(ContractParser::ExprFormulaContext *ctx) override;
        std::any visitExpression(ContractParser::ExpressionContext *ctx) override;
        std::any visitReadOp(ContractParser::ReadOpContext *ctx) override;
        std::any visitWriteOp(ContractParser::WriteOpContext *ctx) override;
        std::any visitCallOp(ContractParser::CallOpContext *ctx) override;
        std::any visitReleaseOp(ContractParser::ReleaseOpContext *ctx) override;

    private:
        std::any aggregateResult(std::any res1, std::any res2) override { return !res1.has_value() ? res2 : res1; };

};
