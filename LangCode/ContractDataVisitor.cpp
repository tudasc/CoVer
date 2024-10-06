#include "ContractDataVisitor.hpp"
#include <any>
#include <memory>
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
    std::shared_ptr<const ReadOperation> opPtr = std::make_shared<ReadOperation>(std::any_cast<const ReadOperation>(this->visit(ctx->primitive())));
    return ContractExpression{ .OP = opPtr};
}

std::any ContractDataVisitor::visitPrimitive(ContractParser::PrimitiveContext *ctx) {
    return ReadOperation(ctx->Variable()->getText());
}
