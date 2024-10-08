#include "ContractDataVisitor.hpp"
#include <any>
#include <memory>
#include <optional>

using namespace ContractTree;

ContractData ContractDataVisitor::getContractData(antlr4::tree::ParseTree* tree) {
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
    std::shared_ptr<const Operation> opPtr = std::any_cast<std::shared_ptr<const Operation>>(this->visit(ctx->primitive()));
    return ContractExpression{ .OP = opPtr};
}

std::any ContractDataVisitor::visitReadOp(ContractParser::ReadOpContext *ctx) {
    std::shared_ptr<const Operation> op = std::make_shared<const ReadOperation>(ReadOperation(ctx->Variable()->getText()));
    return op;
}
std::any ContractDataVisitor::visitWriteOp(ContractParser::WriteOpContext *ctx) {
    std::shared_ptr<const Operation> op = std::make_shared<const WriteOperation>(WriteOperation(ctx->Variable()->getText()));
    return op;
}
