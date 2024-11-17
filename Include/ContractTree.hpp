#pragma once

/* This header defines the C++ side of the contract
 * After parsing the annotation string, the data contained is
 * embedded into a ContractData struct.
 * If you are defining a new verifier, you will need to include this file and the ContractManager.
 */

#include <string>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ContractTree {
    enum struct OperationType { READ, WRITE, CALL, CALLTAG, RELEASE };
    enum struct ParamAccess { NORMAL, DEREF, ADDROF };
    struct Operation {
        virtual ~Operation() = default;
        virtual const OperationType type() const = 0;
    };
    struct RWOperation : Operation {
        const int contrP;
        const ParamAccess contrParamAccess;
        protected:
            RWOperation(int _contrP, ParamAccess _acc) : contrP(_contrP), contrParamAccess(_acc) {};
    };
    struct ReadOperation : RWOperation {
        ReadOperation(int _contrP, ParamAccess _acc) : RWOperation(_contrP, _acc) {};
        virtual const OperationType type() const override { return OperationType::READ; };
    };
    struct WriteOperation : RWOperation {
        WriteOperation(int _contrP, ParamAccess _acc) : RWOperation(_contrP, _acc) {};
        virtual const OperationType type() const override { return OperationType::WRITE; };
    };
    struct CallParam {
        int callP;
        bool callPisTagVar;
        int contrP;
        ParamAccess contrParamAccess;
    };
    struct CallOperation : Operation {
        CallOperation(std::string _func, std::vector<CallParam> _params) : Function{_func}, Params{_params} {};
        const std::string Function;
        const std::vector<CallParam> Params;
        virtual const OperationType type() const override { return OperationType::CALL; };
    };
    struct CallTagOperation : CallOperation {
        CallTagOperation(std::string _func, std::vector<CallParam> _params) : CallOperation(_func, _params) {};
        virtual const OperationType type() const override { return OperationType::CALLTAG; };
    };
    struct ReleaseOperation : Operation {
        ReleaseOperation(std::shared_ptr<const Operation> opNo, std::shared_ptr<const Operation> opUntil) : Forbidden{opNo}, Until{opUntil} {};
        const std::shared_ptr<const Operation> Forbidden;
        const std::shared_ptr<const Operation> Until;
        virtual const OperationType type() const override { return OperationType::RELEASE; };
    };

    enum struct Fulfillment { FULFILLED, UNKNOWN, BROKEN };
    inline const std::string FulfillmentStr(Fulfillment f) { return std::vector<std::string>{ "Fulfilled", "Unknown", "Broken"}[(int)f]; };
    struct ContractExpression {
        const std::shared_ptr<const Operation> OP;
        const std::string ExprStr;
        std::shared_ptr<Fulfillment> Status = std::make_shared<Fulfillment>(Fulfillment::UNKNOWN);
    };

    struct TagUnit {
        std::string tag;
        std::optional<int> param;
    };
    struct ContractData {
        const std::vector<ContractExpression> Pre;
        const std::vector<ContractExpression> Post;
        const std::vector<TagUnit> Tags;
        Fulfillment xres = Fulfillment::UNKNOWN;
    };
}
