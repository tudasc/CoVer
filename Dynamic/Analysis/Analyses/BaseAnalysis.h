#pragma once

#include "../DynamicUtils.h"
#include <unordered_set>

enum struct Fulfillment { FULFILLED, UNKNOWN, VIOLATED };

struct CallBacks {
    bool const FUNCTION: 1;
    bool const MEMORY: 1;
};

class BaseAnalysis {
    public:
        virtual ~BaseAnalysis() = default;

        // Event handlers. Return non-unknown if analysis is resolved and no longer needs to be analysed.
        virtual Fulfillment onFunctionCall(void* location, void* func, CallsiteInfo callsite) { return Fulfillment::UNKNOWN; };
        virtual Fulfillment onMemoryAccess(void* location, void* memory, bool isWrite) { return Fulfillment::UNKNOWN; };
        virtual Fulfillment onProgramExit(void* location) { return Fulfillment::FULFILLED; };

        // For debugging and error output
        virtual std::unordered_set<void*> getReferences() { return {}; };

        // Return which callbacks are needed for this analysis
        virtual CallBacks requiredCallbacks() { return {false, false}; }
};
