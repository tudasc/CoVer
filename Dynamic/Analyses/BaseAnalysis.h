#pragma once

#include "../DynamicUtils.h"
#include <utility>

enum struct Fulfillment { FULFILLED, UNKNOWN, VIOLATED, INACTIVE };

struct CallBacks {
    bool FUNCTION;
    bool MEMORY_R;
    bool MEMORY_W;
};

template<typename T>
class BaseAnalysis {
    protected:
        BaseAnalysis() { references.reserve(10); };
        std::vector<void const*> references;
    public:
        // Event handlers. Return non-unknown if analysis is resolved and no longer needs to be analysed.
        // onFunctionCall does not forward return address, as it is included in callsiteinfo
        inline Fulfillment onFunctionCall(CodePtr const& location, void* const& func, CallsiteInfo const& callsite) { return static_cast<T*>(this)->functionCBImpl(func, callsite); };
        inline Fulfillment onMemoryAccess(CodePtr const& location, void const* const& memory, bool const& isWrite) { return static_cast<T*>(this)->memoryCBImpl(std::forward<CodePtr const>(location), memory, isWrite); };
        inline Fulfillment onProgramExit(CodePtr const& location) { return static_cast<T*>(this)->exitCBImpl(std::forward<void const* const>(location)); };

        // For debugging and error output
        inline std::vector<CodePtr> const& getReferences() { return std::move(references); };

        // Return which callbacks are needed for this analysis
        CallBacks requiredCallbacks() const { return static_cast<T const*>(this)->requiredCallbacksImpl(); }
};
