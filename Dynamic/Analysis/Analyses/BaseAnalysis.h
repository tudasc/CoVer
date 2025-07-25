#pragma once

#include <vector>
#include "../DynamicUtils.h"

enum struct Fulfillment { FULFILLED, UNKNOWN, VIOLATED };

class BaseAnalysis {
    public:
        virtual ~BaseAnalysis() = default;

        // Event handlers. Return true if analysis is resolved and no longer needs to be analysed.
        virtual Fulfillment onFunctionCall(void* location, void* func, CallsiteParams params) { return Fulfillment::FULFILLED; };
        virtual Fulfillment onProgramExit(void* location) { return Fulfillment::FULFILLED; };
};
