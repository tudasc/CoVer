#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <vector>

struct ReleaseAnalysis : BaseAnalysis<ReleaseAnalysis> {
    public:
        ReleaseAnalysis(void* func_supplier, ReleaseOp_t* rOP);
        inline __attribute__((always_inline)) Fulfillment functionCBImpl(void* const& func, CallsiteInfo const& callsite);
        inline __attribute__((always_inline)) Fulfillment memoryCBImpl(void const* const& location, void const* const& memory, bool const& isWrite);
        inline __attribute__((always_inline)) Fulfillment exitCBImpl(void const* const& location) const { return Fulfillment::FULFILLED; };

        CallBacks requiredCallbacksImpl() const;

    private:
        // Configuration
        void* func_supplier;
        bool forbIsRW = false;
        ParamAccess rwAcc;
        int32_t rwIdx;
        void* forbiddenOp;
        std::string target_str_forb; // Either tag str or func str
        std::unordered_set<void*> forb_funcs;
        std::vector<CallParam_t*> params_forb;
        std::string target_str_rel; // Either tag str or func str
        std::unordered_set<void*> rel_funcs;
        std::vector<CallParam_t*> params_release; // Required parameters

        // Analysis temporaries
        std::vector<CallsiteInfo> forbiddenCallsites;
        std::vector<void const*> forbMem;
};
