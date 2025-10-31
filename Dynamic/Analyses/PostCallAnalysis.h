#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <vector>

struct PostCallAnalysis : public BaseAnalysis<PostCallAnalysis> {
    public:
        PostCallAnalysis(void* func_supplier, CallOp_t* callop);
        PostCallAnalysis(void* func_supplier, CallTagOp_t* callop);

        inline __attribute__((always_inline)) Fulfillment functionCBImpl(void* const& func, CallsiteInfo const& callsite);
        inline __attribute__((always_inline)) Fulfillment memoryCBImpl(void const* const& location, void const* const& memory, bool const& isWrite) const { return Fulfillment::UNKNOWN; }
        inline __attribute__((always_inline)) Fulfillment exitCBImpl(void const* const& location);

        constexpr CallBacks requiredCallbacksImpl() const { return {true, false, false}; }

    private:
        void SharedInit(void* _func_supplier, const char* target_str, CallParam_t *params, int64_t num_params);

        // Configuration
        void* func_supplier;
        std::string target_str; // Either tag str or func str
        std::vector<CallParam_t*> params; // Required parameters
        std::unordered_set<void*> target_funcs;

        // Analysis temporaries
        std::vector<CallsiteInfo> uncheckedCallsites;
};
