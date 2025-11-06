#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <string>
#include <vector>

struct ReleaseAnalysis : BaseAnalysis<ReleaseAnalysis> {
    public:
        ReleaseAnalysis(void const* func_supplier, ReleaseOp_t* rOP);
        inline __attribute__((always_inline)) Fulfillment functionCBImpl(void* const& func, CallsiteInfo const& callsite);
        inline __attribute__((always_inline)) Fulfillment memoryCBImpl(void const* const& location, void const* const& memory, bool const& isWrite);
        inline __attribute__((always_inline)) Fulfillment exitCBImpl(void const* const& location) const { return Fulfillment::FULFILLED; };

        CallBacks requiredCallbacksImpl() const;

    private:
        // Configuration
        void const* func_supplier;
        bool forbIsRW = false;
        ParamAccess rwAcc;
        int32_t rwIdx;
        void const* forbiddenOp;
        std::string target_str_forb; // Either tag str or func str
        std::vector<void const*> forb_funcs;
        std::vector<CallParam_t*> params_forb;
        std::string target_str_rel; // Either tag str or func str
        std::vector<void const*> rel_funcs;
        std::vector<CallParam_t*> params_release; // Required parameters

        // Analysis temporaries
        std::vector<CallsiteInfo> forbiddenCallsites;
        std::vector<ConcreteParam> forbMem;
};
