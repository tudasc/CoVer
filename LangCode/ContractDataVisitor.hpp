#pragma once

#include <antlr4-runtime.h>

#include "ContractParserBaseVisitor.h"
#include "tree/ParseTree.h"

#include "ContractTree.hpp"

class ContractDataVisitor : public ContractParserBaseVisitor {
    public:
        std::any visitContract(ContractParser::ContractContext *ctx) override;
        std::any visitExpression(ContractParser::ExpressionContext *ctx) override;
        std::any visitReadOp(ContractParser::ReadOpContext *ctx) override;
        std::any visitWriteOp(ContractParser::WriteOpContext *ctx) override;
        std::any visitCallOp(ContractParser::CallOpContext *ctx) override;
        std::any visitReleaseOp(ContractParser::ReleaseOpContext *ctx) override;

        ContractTree::ContractData getContractData(antlr4::tree::ParseTree* tree);

    private:
        std::any aggregateResult(std::any res1, std::any res2) override { return !res1.has_value() ? res2 : res1; };

};
