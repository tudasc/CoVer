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
    std::vector<ContractExpression> preExprs;
    if (ctx->precondition()) {
        preExprs = std::any_cast<std::vector<ContractExpression>>(this->visit(ctx->precondition()->exprList()));
    }

    std::vector<ContractExpression> postExprs;
    if (ctx->postcondition()) {
        postExprs = std::any_cast<std::vector<ContractExpression>>(this->visit(ctx->postcondition()->exprList()));
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

    return ContractData{preExprs, postExprs, tags, f};
}

std::any ContractDataVisitor::visitExprList(ContractParser::ExprListContext *ctx) {
    std::vector<ContractExpression> exprs;
    for (ContractParser::ExpressionContext* exprctx : ctx->expression()) {
        exprs.push_back(std::any_cast<ContractExpression>(this->visit(exprctx)));
    }
    return exprs;
}
std::any ContractDataVisitor::visitExpression(ContractParser::ExpressionContext *ctx) {
    std::shared_ptr<const Operation> opPtr;
    opPtr = std::any_cast<std::shared_ptr<const Operation>>(this->visitChildren(ctx));
    return ContractExpression{ opPtr, ctx->getText()};
}

std::any ContractDataVisitor::visitReadOp(ContractParser::ReadOpContext *ctx) {
    ParamAccess acc = ParamAccess::NORMAL;
        if (ctx->Deref())
            acc = ParamAccess::DEREF;
        if (ctx->AddrOf())
            acc = ParamAccess::ADDROF;
    std::shared_ptr<const Operation> op = std::make_shared<const ReadOperation>(ReadOperation(std::stoi(ctx->NatNum()->getText()), acc));
    return op;
}
std::any ContractDataVisitor::visitWriteOp(ContractParser::WriteOpContext *ctx) {
    ParamAccess acc = ParamAccess::NORMAL;
        if (ctx->Deref())
            acc = ParamAccess::DEREF;
        if (ctx->AddrOf())
            acc = ParamAccess::ADDROF;
    std::shared_ptr<const Operation> op = std::make_shared<const WriteOperation>(WriteOperation(std::stoi(ctx->NatNum()->getText()), acc));
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
