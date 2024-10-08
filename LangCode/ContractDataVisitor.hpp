#pragma once

#include <memory>
#include <optional>

#include <antlr4-runtime.h>

#include "ContractParserBaseVisitor.h"
#include "tree/ParseTree.h"

namespace ContractTree {
    enum struct OperationType { READ, WRITE };
    struct Operation {
            virtual ~Operation() = default;
            virtual const OperationType type() const = 0;
        };
        struct ReadOperation : Operation {
            ReadOperation(std::string _var) : Variable{_var} {};
            const std::string Variable;
            virtual const OperationType type() const override { return OperationType::READ; };
        };
        struct WriteOperation : Operation {
            WriteOperation(std::string _var) : Variable{_var} {};
            const std::string Variable;
            virtual const OperationType type() const override { return OperationType::WRITE; };
        };

        struct ContractExpression {
            const std::shared_ptr<const Operation> OP;
        };

        struct ContractData {
            const std::optional<ContractExpression> Pre;
            const std::optional<ContractExpression> Post;
        };
}

class ContractDataVisitor : public ContractParserBaseVisitor {
    public:
        std::any visitContract(ContractParser::ContractContext *ctx) override;
        std::any visitExpression(ContractParser::ExpressionContext *ctx) override;
        std::any visitReadOp(ContractParser::ReadOpContext *ctx) override;
        std::any visitWriteOp(ContractParser::WriteOpContext *ctx) override;

        ContractTree::ContractData getContractData(antlr4::tree::ParseTree* tree);

    private:
        std::any aggregateResult(std::any res1, std::any res2) override { return !res1.has_value() ? res2 : res1; };
        
};
