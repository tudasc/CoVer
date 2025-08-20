#pragma once

#include "../DynamicUtils.h"
#include <utility>

enum struct Fulfillment { FULFILLED, UNKNOWN, VIOLATED };

struct CallBacks {
    bool const FUNCTION;
    bool const MEMORY_R;
    bool const MEMORY_W;
};

template<typename T>
class BaseAnalysis {
    protected:
        BaseAnalysis() { references.reserve(10); };
        std::vector<void const*> references;
    public:
        // Event handlers. Return non-unknown if analysis is resolved and no longer needs to be analysed.
        // onFunctionCall does not forward return address, as it is included in callsiteinfo
        inline Fulfillment onFunctionCall(void const* const&& location, void* const& func, CallsiteInfo const& callsite) { return static_cast<T*>(this)->functionCBImpl(func, callsite); };
        inline Fulfillment onMemoryAccess(void const* const&& location, void* const& memory, bool const& isWrite) { return static_cast<T*>(this)->memoryCBImpl(std::forward<void const* const>(location), memory, isWrite); };
        inline Fulfillment onProgramExit(void const* const&& location) { return static_cast<T*>(this)->exitCBImpl(std::forward<void const* const>(location)); };

        // For debugging and error output
        inline std::vector<void const*> const&& getReferences() { return std::move(references); };

        // Return which callbacks are needed for this analysis
        CallBacks requiredCallbacks() { return static_cast<T*>(this)->requiredCallbacksImpl(); }
};
