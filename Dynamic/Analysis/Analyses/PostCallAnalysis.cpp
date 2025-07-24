#include "PostCallAnalysis.h"
#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include "../DynamicUtils.h"

#include <vector>

void PostCallAnalysis::SharedInit(void* _func_supplier, const char* _target_str, CallParam_t *_params, int64_t num_params) {
    func_supplier = _func_supplier;
    target_str = _target_str;
    for (int i = 0; i < num_params; i++) {
        params.push_back(&_params[i]);
    }
}

PostCallAnalysis::PostCallAnalysis(void* _func_supplier, CallOp_t* callop) {
    SharedInit(_func_supplier, callop->function_name, callop->params, callop->num_params);
    target_funcs = {callop->target_function};
}
PostCallAnalysis::PostCallAnalysis(void* _func_supplier, CallTagOp_t* callop) {
    SharedInit(_func_supplier, callop->target_tag, callop->params, callop->num_params);
    target_funcs = DynamicUtils::getFunctionsForTag(callop->target_tag);
}

Fulfillment PostCallAnalysis::onFunctionCall(void* location, void* func, CallsiteParams callsite_params) {
    if (target_funcs.contains(func)) {
        // Target function found, maybe analysis success

        // Check params if needed
        if (params.empty()) {
            uncheckedCallsites.clear();
            return Fulfillment::UNKNOWN; // Cannot return fulfilled until program exit, there may be more callsites to come
        }

        // Check which callsites are satisfied, remove from unchecked
        for (CallParam_t* param : params) {
            for (auto callsite_iter = uncheckedCallsites.begin(); callsite_iter != uncheckedCallsites.end();) {
                for (CallsiteParams supplier_params : callsite_iter->second) {
                    if (param->callPisTagVar) {
                        std::set<Tag_t> tags = DynamicUtils::getTagsForFunction(func);
                        for (Tag_t tag : tags) {
                            if (tag.tag != target_str) continue;
                            if (DynamicUtils::checkParamMatch(param->accType, supplier_params[param->contrP].val, callsite_params[tag.param].val)) {
                                callsite_iter = uncheckedCallsites.erase(callsite_iter);
                                goto callsite_clear;
                            }
                        }
                    } else {
                        if (DynamicUtils::checkParamMatch(param->accType, supplier_params[param->contrP].val, callsite_params[param->callP].val)) {
                            callsite_iter = uncheckedCallsites.erase(callsite_iter);
                            goto callsite_clear;
                        }
                    }
                }
                callsite_iter++;
                callsite_clear:;
            }
        }
        // For the rest: Maybe actual fulfillment comes later
        return Fulfillment::UNKNOWN;
    } else if (func == func_supplier) {
        uncheckedCallsites[location].push_back(callsite_params);
    }

    // Irrelevant function
    return Fulfillment::UNKNOWN;
}

Fulfillment PostCallAnalysis::onProgramExit(void* location) {
    if (!uncheckedCallsites.empty()) {
        DynamicUtils::createMessage("Postcall");
        return Fulfillment::VIOLATED;
    }
    return Fulfillment::FULFILLED;
}
