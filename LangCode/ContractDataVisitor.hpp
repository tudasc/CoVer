#pragma once

#include <optional>

#include <antlr4-runtime.h>

#include "ContractParserBaseVisitor.h"
#include "tree/ParseTree.h"

class ContractDataVisitor : public ContractParserBaseVisitor {
    public:
        struct Operation {
            virtual ~Operation() = default;
            virtual const std::string type() const;
        };
        struct ReadOperation : Operation {
            ReadOperation(std::string _var) : Variable{_var} {};
            const std::string Variable;
            const std::string type() const override { return "ReadOperation"; };
        };

        struct ContractExpression {
            const Operation& OP;
        };

        struct ContractData {
            const std::optional<ContractExpression> Pre;
            const std::optional<ContractExpression> Post;
        };

        std::any visitContract(ContractParser::ContractContext *ctx) override;
        std::any visitExpression(ContractParser::ExpressionContext *ctx) override;
        std::any visitPrimitive(ContractParser::PrimitiveContext *ctx) override;

        ContractData getContractData(antlr4::tree::ParseTree* tree);

    private:
        std::any aggregateResult(std::any res1, std::any res2) override { return !res1.has_value() ? res2 : res1; };
        
};
