#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <unordered_set>
#include <string>
#include <vector>

struct PostCallAnalysis : public BaseAnalysis {
    public:
        PostCallAnalysis(void* func_supplier, CallOp_t* callop);
        PostCallAnalysis(void* func_supplier, CallTagOp_t* callop);

        Fulfillment functionCBImpl(void* const& func, CallsiteInfo const& callsite);
        Fulfillment memoryCBImpl(void const* const&& location, void* const& memory, bool const& isWrite) { return Fulfillment::UNKNOWN; }
        Fulfillment exitCBImpl(void const* const&& location);

        CallBacks requiredCallbacksImpl() { return {true, false, false}; }

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
