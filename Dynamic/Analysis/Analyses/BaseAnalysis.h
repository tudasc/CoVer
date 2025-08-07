#pragma once

#include "../DynamicUtils.h"
#include <unordered_set>

enum struct Fulfillment { FULFILLED, UNKNOWN, VIOLATED };

class BaseAnalysis {
    public:
        virtual ~BaseAnalysis() = default;

        // Event handlers. Return true if analysis is resolved and no longer needs to be analysed.
        // Default return unknown except on exit.
        virtual Fulfillment onFunctionCall(void* location, void* func, CallsiteParams params) { return Fulfillment::UNKNOWN; };
        virtual Fulfillment onMemoryAccess(void* location, void* memory, bool isWrite) { return Fulfillment::UNKNOWN; };
        virtual Fulfillment onProgramExit(void* location) { return Fulfillment::FULFILLED; };

        // For debugging and error output
        virtual std::unordered_set<void*> getReferences() { return {}; };
};
