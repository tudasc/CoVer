#include "ContractDataVisitor.hpp"
#include <any>
#include <optional>

ContractDataVisitor::ContractData ContractDataVisitor::getContractData(antlr4::tree::ParseTree* tree) {
    return std::any_cast<ContractData>(this->visit(tree));
}

std::any ContractDataVisitor::visitContract(ContractParser::ContractContext *ctx) {
    std::optional<ContractExpression> preExpr;
    if (ctx->precondition()) {
        std::any expr = this->visit(ctx->precondition());
        preExpr.emplace(std::any_cast<ContractExpression>(expr));
    }

    std::optional<ContractExpression> postExpr;
    if (ctx->postcondition()) {
        std::any expr = this->visit(ctx->postcondition());
        postExpr.emplace(std::any_cast<ContractExpression>(expr));
    }

    return ContractData{preExpr, postExpr};
}

std::any ContractDataVisitor::visitExpression(ContractParser::ExpressionContext *ctx) {
    return ContractExpression{ .OP = std::any_cast<ReadOperation>(this->visit(ctx->primitive())) };
}

std::any ContractDataVisitor::visitPrimitive(ContractParser::PrimitiveContext *ctx) {
    return ReadOperation(ctx->Variable()->getText());
}
