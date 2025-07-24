#pragma once

#include <vector>

struct ConcreteParam {
    void* val;
    bool isPtr;
};
using CallsiteParams = std::vector<ConcreteParam>;

enum struct Fulfillment { FULFILLED, UNKNOWN, VIOLATED };

class BaseAnalysis {
    public:
        virtual ~BaseAnalysis() = default;

        // Event handlers. Return true if analysis is resolved and no longer needs to be analysed.
        virtual Fulfillment onFunctionCall(void* location, void* func, CallsiteParams params) { return Fulfillment::FULFILLED; };
        virtual Fulfillment onProgramExit(void* location) { return Fulfillment::FULFILLED; };
};
