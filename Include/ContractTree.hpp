#pragma once

/* This header defines the C++ side of the contract
 * After parsing the annotation string, the data contained is
 * embedded into a ContractData struct.
 * If you are defining a new verifier, you will need to include this file and the ContractManager.
 */

#include <string>
#include <memory>
#include <optional>

namespace ContractTree {
    enum struct OperationType { READ, WRITE, CALL };
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
    struct CallOperation : Operation {
        CallOperation(std::string _func) : Function{_func} {};
        const std::string Function;
        virtual const OperationType type() const override { return OperationType::CALL; };
    };

    struct ContractExpression {
        const std::shared_ptr<const Operation> OP;
    };

    enum struct Fulfillment { UNKNOWN, FULFILLED, BROKEN };
    struct ContractData {
        const std::optional<ContractExpression> Pre;
        const std::optional<ContractExpression> Post;
        Fulfillment xres = Fulfillment::UNKNOWN;
    };
}
