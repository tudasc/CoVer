#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <map>
#include <set>
#include <string>
#include <vector>

struct PreCallAnalysis : BaseAnalysis {
    public:
        PreCallAnalysis(void* func_supplier, CallOp_t* callop);
        PreCallAnalysis(void* func_supplier, CallTagOp_t* callop);
        virtual Fulfillment onFunctionCall(void* location, void* func,  CallsiteParams params) override;

    private:
        void SharedInit(void* _func_supplier, const char* target_str, CallParam_t *params, int64_t num_params);

        // Configuration
        void* func_supplier;
        std::string target_str; // Either tag str or func str
        std::vector<CallParam_t*> params; // Required parameters
        std::set<void*> target_funcs;

        // Analysis temporaries
        std::map<void*, std::vector<CallsiteParams>> possible_matches;
};
