#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

struct ReleaseAnalysis : BaseAnalysis {
    public:
        ReleaseAnalysis(void* func_supplier, ReleaseOp_t* rOP);
        virtual Fulfillment onFunctionCall(void* location, void* func,  CallsiteParams params) override;
        virtual Fulfillment onMemoryAccess(void* location, void* memory, bool isWrite) override;
        virtual std::unordered_set<void*> getReferences() override { return references; };

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
        std::unordered_map<void*, std::vector<CallsiteParams>> forbiddenCallsites;

        std::unordered_set<void*> references;
};
