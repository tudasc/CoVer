#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <unordered_set>
#include <string>
#include <vector>

struct ReleaseAnalysis : BaseAnalysis {
    public:
        ReleaseAnalysis(void* func_supplier, ReleaseOp_t* rOP);
        Fulfillment functionCBImpl(void* const&& location, void* const& func, CallsiteInfo const& callsite);
        Fulfillment memoryCBImpl(void* const&& location, void* const& memory, bool const& isWrite);
        Fulfillment exitCBImpl(void* const&& location) { return Fulfillment::FULFILLED; };
        inline std::unordered_set<void*> getReferenceImpl() { return references; };

        CallBacks requiredCallbacksImpl();

    private:
        // Configuration
        void* func_supplier;
        bool forbIsRW = false;
        void* forbiddenOp;
        int64_t forbidden_kind;
        std::string target_str_forb; // Either tag str or func str
        std::unordered_set<void*> forb_funcs;
        std::vector<CallParam_t*> params_forb;
        std::string target_str_rel; // Either tag str or func str
        std::unordered_set<void*> rel_funcs;
        std::vector<CallParam_t*> params_release; // Required parameters

        // Analysis temporaries
        std::vector<CallsiteInfo> forbiddenCallsites;

        std::unordered_set<void*> references;
};
