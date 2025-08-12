#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

struct PreCallAnalysis : BaseAnalysis {
    public:
        PreCallAnalysis(void* func_supplier, CallOp_t* callop);
        PreCallAnalysis(void* func_supplier, CallTagOp_t* callop);

        Fulfillment functionCBImpl(void* const&& location, void* const& func, CallsiteInfo const& callsite);
        Fulfillment memoryCBImpl(void* const&& location, void* const& memory, bool const& isWrite) { return Fulfillment::UNKNOWN; }
        Fulfillment exitCBImpl(void* const&& location) { return Fulfillment::FULFILLED; };

        std::unordered_set<void*> getReferenceImpl() { return references; };

        CallBacks requiredCallbacksImpl() { return {true, false}; }

    private:
        void SharedInit(void* _func_supplier, const char* target_str, CallParam_t *params, int64_t num_params);

        // Configuration
        void* func_supplier;
        std::string target_str; // Either tag str or func str
        std::vector<CallParam_t*> params; // Required parameters
        std::unordered_set<void*> target_funcs;

        // Analysis temporaries
        std::unordered_map<void*, std::vector<CallsiteInfo>> possible_matches;

        std::unordered_set<void*> references;
};
