#include "ContractDataVisitor.hpp"
#include "ContractLangErrorListener.hpp"
#include "ContractParser.h"
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

    std::vector<TagUnit> tags;
    if (ctx->functags()) {
        for (ContractParser::TagUnitContext* tagUnitCtx : ctx->functags()->tagUnit()) {
            TagUnit t;
            t.tag = tagUnitCtx->Variable()->getText();
            if (tagUnitCtx->NatNum()) t.param = std::stoi(tagUnitCtx->NatNum()->getText());
            tags.push_back(t);
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
    std::vector<CallParam> params;
    for (ContractParser::VarMapContext* param : ctx->varMap()) {
        bool isTagVar = param->TagParam() ? true : false;
        if (ctx->OPCall() && isTagVar) {
            // This is an error! Call is not a tag call, but parameter access requested
            throw ContractLangSyntaxError(param->TagParam()->getSymbol()->getLine(), param->TagParam()->getSymbol()->getCharPositionInLine(), "Attempted to use tag param in normal call!");
        }
        ParamAccess acc = ParamAccess::NORMAL;
        if (param->Deref())
            acc = ParamAccess::DEREF;
        if (param->AddrOf())
            acc = ParamAccess::ADDROF;
        params.push_back({std::stoi(param->callP ? param->callP->getText() : "-1"), isTagVar, std::stoi(param->contrP->getText()), acc});
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
