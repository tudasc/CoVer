#pragma once

#include "../DynamicUtils.h"
#include <unordered_set>
#include <utility>

enum struct Fulfillment { FULFILLED, UNKNOWN, VIOLATED };

struct CallBacks {
    bool const FUNCTION;
    bool const MEMORY_R;
    bool const MEMORY_W;
};

class BaseAnalysis {
    protected:
        BaseAnalysis() = default;
    public:
        // Event handlers. Return non-unknown if analysis is resolved and no longer needs to be analysed.
        inline Fulfillment onFunctionCall(this auto& self, void* const&& location, void* const& func, CallsiteInfo const& callsite) { return self.functionCBImpl(std::move(location), func, callsite); };
        inline Fulfillment onMemoryAccess(this auto& self, void* const&& location, void* const& memory, bool const& isWrite) { return self.memoryCBImpl(std::move(location), memory, isWrite); };
        inline Fulfillment onProgramExit(this auto& self, void* const&& location) { return self.exitCBImpl(std::move(location)); };

        // For debugging and error output
        inline std::unordered_set<void*> getReferences(this auto& self) { return self.getReferenceImpl(); };

        // Return which callbacks are needed for this analysis
        CallBacks requiredCallbacks(this auto& self) { return self.requiredCallbacksImpl(); }
};
