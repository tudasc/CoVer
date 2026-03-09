#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <string>
#include <vector>

struct PostCallAnalysis : public BaseAnalysis<PostCallAnalysis> {
    public:
        PostCallAnalysis(void const* func_supplier, CallOp_t* callop);
        PostCallAnalysis(void const* func_supplier, CallTagOp_t* callop);

        ANALYSIS_PREAMBLE Fulfillment functionCBImpl(void* const& func, CallsiteInfo const& callsite);
        ANALYSIS_PREAMBLE Fulfillment memoryCBImpl(CodePtr const& location, void const* const& memory, bool const& isWrite) const { return Fulfillment::UNKNOWN; }
        ANALYSIS_PREAMBLE Fulfillment exitCBImpl(CodePtr const& location);

        constexpr CallBacks requiredCallbacksImpl() const { return {true, false, false}; }

    private:
        void SharedInit(void const* _func_supplier, const char* target_str, CallParam_t *params, int64_t num_params);

        // Configuration
        void const* func_supplier;
        std::string target_str; // Either tag str or func str
        std::vector<CallParam_t*> params; // Required parameters
        std::vector<void const*> target_funcs;

        // Analysis temporaries
        std::vector<CallsiteInfo> uncheckedCallsites;
};
