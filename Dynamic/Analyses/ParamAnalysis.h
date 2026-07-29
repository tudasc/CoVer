#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <vector>

struct ParamAnalysis : BaseAnalysis<ParamAnalysis> {
    public:
        ParamAnalysis(void const* _func_supplier, ParamOp_t* paramop) : idx(paramop->idx), func_supplier(_func_supplier), callval_need_deref(paramop->callval_need_deref) {
            for (int i = 0; i < paramop->num_reqs; i++) {
                param_requirements.push_back(&paramop->requirements[i]);
            }
        }

        ANALYSIS_PREAMBLE Fulfillment functionPreCBImpl(void* const& func, CallsiteInfo const& callsite);
        ANALYSIS_PREAMBLE Fulfillment functionPostCBImpl(void* const& func, CallsiteInfo const& callsite) { return Fulfillment::UNKNOWN; };
        ANALYSIS_PREAMBLE Fulfillment memoryCBImpl(CodePtr const& location, void const* const& memory, bool const& isWrite) const { return Fulfillment::UNKNOWN; }
        ANALYSIS_PREAMBLE Fulfillment exitCBImpl(CodePtr const& location) const { return Fulfillment::FULFILLED; }; // Evidently none of the callsites were erroneous 

        constexpr CallBacks requiredCallbacksImpl() const { return {true, false, false, false}; }

    private:
        // Configuration
        void const* func_supplier;
        int const idx;
        bool const callval_need_deref;
        std::vector<ParamReq_t const*> param_requirements;
};
