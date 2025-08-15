#include "PreCallAnalysis.h"
#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include "../DynamicUtils.h"

#include <vector>

void PreCallAnalysis::SharedInit(void* _func_supplier, const char* _target_str, CallParam_t *_params, int64_t num_params) {
    func_supplier = _func_supplier;
    target_str = _target_str;
    for (int i = 0; i < num_params; i++) {
        params.push_back(&_params[i]);
    }
}

PreCallAnalysis::PreCallAnalysis(void* _func_supplier, CallOp_t* callop) {
    SharedInit(_func_supplier, callop->function_name, callop->params, callop->num_params);
    target_funcs = {callop->target_function};
}
PreCallAnalysis::PreCallAnalysis(void* _func_supplier, CallTagOp_t* callop) {
    SharedInit(_func_supplier, callop->target_tag, callop->params, callop->num_params);
    target_funcs = DynamicUtils::getFunctionsForTag(callop->target_tag);
}

Fulfillment PreCallAnalysis::functionCBImpl(void* const&& location, void* const& func, CallsiteInfo const& callsite) {
    if (target_funcs.contains(func)) {
        // Possible match for precall
        possible_matches[func].push_back(callsite);
        return Fulfillment::UNKNOWN;
    } else if (func == func_supplier) {
        // Contract supplier found, need to resolve now
        if (possible_matches.empty()) {
            // No matches, verification failed
            references.push_back(location);
            return Fulfillment::VIOLATED;
        }

        // Check params if needed
        if (params.empty()) return Fulfillment::FULFILLED;
        for (std::pair<void*,std::vector<CallsiteInfo>> possible_match : possible_matches) {
            for (CallsiteInfo match_params : possible_match.second) {
                if (DynamicUtils::checkFuncCallMatch(possible_match.first, params, match_params, callsite, target_str)) {
                    // Success!
                    return Fulfillment::FULFILLED;
                }
            }
        }

        // Nothing matched
        references.push_back(location);
        return Fulfillment::VIOLATED;
    }

    // Irrelevant function
    return Fulfillment::UNKNOWN;
}
