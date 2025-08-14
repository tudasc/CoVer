#include "ReleaseAnalysis.h"
#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include "../DynamicUtils.h"

#include <vector>

namespace {
    void processFunctionForInit(std::string& target, const char* target_orig, std::vector<CallParam_t *>& param_new, int num_params, CallParam_t* params) {
        for (int i = 0; i < num_params; i++) {
            param_new.push_back(&params[i]);
        }
        target = target_orig;
    }
}

ReleaseAnalysis::ReleaseAnalysis(void* _func_supplier, ReleaseOp_t* rOP) {
    if (rOP->forbidden_op_kind == UNARY_READ || rOP->forbidden_op_kind == UNARY_WRITE) forbIsRW = true;
    else {
        if (rOP->forbidden_op_kind == UNARY_CALLTAG) {
            CallTagOp_t* cOP = (CallTagOp_t*)rOP->forbidden_op;
            processFunctionForInit(target_str_forb, cOP->target_tag, params_forb, cOP->num_params, cOP->params);
            forb_funcs = DynamicUtils::getFunctionsForTag(cOP->target_tag);
        } else {
            CallOp_t* cOP = (CallOp_t*)rOP->forbidden_op;
            processFunctionForInit(target_str_forb, cOP->function_name, params_forb, cOP->num_params, cOP->params);
            forb_funcs = {cOP->target_function};
        }
    }
    forbiddenOp = rOP->forbidden_op;

    if (rOP->release_op_kind == UNARY_CALLTAG) {
        CallTagOp_t* cOP = (CallTagOp_t*)rOP->release_op;
        processFunctionForInit(target_str_rel, cOP->target_tag, params_release, cOP->num_params, cOP->params);
        rel_funcs = DynamicUtils::getFunctionsForTag(cOP->target_tag);
    } else {
        CallOp_t* cOP = (CallOp_t*)rOP->release_op;
        processFunctionForInit(target_str_rel, cOP->function_name, params_release, cOP->num_params, cOP->params);
        rel_funcs = {cOP->target_function};
    }

    func_supplier = _func_supplier;
}

CallBacks ReleaseAnalysis::requiredCallbacksImpl() {
    if (!forbIsRW) return {true, false, false};
    RWOp_t* rwOp = (RWOp_t*)forbiddenOp;
    return {true, !rwOp->isWrite, (bool)rwOp->isWrite};
}

Fulfillment ReleaseAnalysis::functionCBImpl(void* const&& location, void* const& func, CallsiteInfo const& callsite) {
    // First, check if release
    if (rel_funcs.contains(func)) {
        if (params_release.empty()) {
            forbiddenCallsites.clear();
            return Fulfillment::UNKNOWN;
        }
        // Check which callsites are satisfied, remove from unchecked
        for (auto callsite_iter = forbiddenCallsites.begin(); callsite_iter != forbiddenCallsites.end();) {
            if (DynamicUtils::checkFuncCallMatch(func, params_release, callsite, *callsite_iter, target_str_rel)) {
                callsite_iter = forbiddenCallsites.erase(callsite_iter);
            } else {
                callsite_iter++;
            }
        }
        // For the rest: Maybe actual fulfillment comes later
        return Fulfillment::UNKNOWN;
    }

    // Check if forbidden
    if (forb_funcs.contains(func)) {
        if (params_forb.empty()) {
            references.insert(location);
            for (CallsiteInfo const& callsite : forbiddenCallsites) references.insert(callsite.location);
            return Fulfillment::VIOLATED;
        }

        // Check if a callsite is violated
        for (CallsiteInfo const& callsite : forbiddenCallsites) {
            if (DynamicUtils::checkFuncCallMatch(func, params_forb, callsite, callsite, target_str_forb)) {
                references.insert(location);
                references.insert(callsite.location);
                return Fulfillment::VIOLATED;
            }
        }
    }

    // Finally, check if supplier.
    // Needs to be done after check for forbidden, so that new supplier is not accidentally checked against itself
    if (func == func_supplier) {
        forbiddenCallsites.push_back(callsite);
    }

    // Irrelevant function
    return Fulfillment::UNKNOWN;
}

Fulfillment ReleaseAnalysis::memoryCBImpl(void* const&& location, void* const& memory, bool const& isWrite) {
    RWOp_t* rwOp = (RWOp_t*)forbiddenOp;

    for (CallsiteInfo const& callsite : forbiddenCallsites) {
        if (DynamicUtils::checkParamMatch(rwOp->accType, &callsite.params[rwOp->idx], memory)) {
            references.insert(location);
            references.insert(callsite.location);
            return Fulfillment::VIOLATED;
        }
    }

    return Fulfillment::UNKNOWN;
}
