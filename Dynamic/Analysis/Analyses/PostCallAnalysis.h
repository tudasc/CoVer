#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

struct PostCallAnalysis : BaseAnalysis {
    public:
        PostCallAnalysis(void* func_supplier, CallOp_t* callop);
        PostCallAnalysis(void* func_supplier, CallTagOp_t* callop);
        virtual Fulfillment onFunctionCall(void* location, void* func,  CallsiteParams params) override;
        virtual Fulfillment onProgramExit(void* location) override;

    private:
        void SharedInit(void* _func_supplier, const char* target_str, CallParam_t *params, int64_t num_params);

        // Configuration
        void* func_supplier;
        std::string target_str; // Either tag str or func str
        std::vector<CallParam_t*> params; // Required parameters
        std::unordered_set<void*> target_funcs;

        // Analysis temporaries
        std::unordered_map<void*, std::vector<CallsiteParams>> uncheckedCallsites;
};
