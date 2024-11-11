#include "ContractDataVisitor.hpp"
#include "ContractTree.hpp"
#include <any>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

    std::vector<std::string> tags;
    if (ctx->functags()) {
        for (antlr4::tree::TerminalNode* tag : ctx->functags()->Variable()) {
            tags.push_back(tag->toString());
        }
    }

    Fulfillment f = Fulfillment::UNKNOWN;
    if (ctx->ContractMarkerExpFail()) f = Fulfillment::BROKEN;
    if (ctx->ContractMarkerExpSucc()) f = Fulfillment::FULFILLED;

    return ContractData{preExpr, postExpr, tags, f};
}

std::any ContractDataVisitor::visitExpression(ContractParser::ExpressionContext *ctx) {
    std::shared_ptr<const Operation> opPtr;
    opPtr = std::any_cast<std::shared_ptr<const Operation>>(this->visitChildren(ctx));
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
std::any ContractDataVisitor::visitCallOp(ContractParser::CallOpContext *ctx) {
    std::vector<int> params;
    for (antlr4::tree::TerminalNode* param : ctx->NatNum()) {
        params.push_back(std::stoi(param->toString()));
    }
    std::shared_ptr<const Operation> op;
    if (ctx->OPCall())
        op = std::make_shared<const CallOperation>(CallOperation(ctx->Variable()->getText(), params));
    else
        op = std::make_shared<const CallTagOperation>(CallTagOperation(ctx->Variable()->getText(), params));
    return op;
}
std::any ContractDataVisitor::visitReleaseOp(ContractParser::ReleaseOpContext *ctx) {
    std::shared_ptr<const Operation> opForbidden = std::any_cast<std::shared_ptr<const Operation>>(this->visit(ctx->forbidden));
    std::shared_ptr<const Operation> opUntil = std::any_cast<std::shared_ptr<const Operation>>(this->visit(ctx->until));

    std::shared_ptr<const Operation> op = std::make_shared<const ReleaseOperation>(ReleaseOperation(opForbidden, opUntil));
    return op;
}
