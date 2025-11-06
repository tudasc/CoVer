#include "ReleaseAnalysis.h"
#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include "../DynamicUtils.h"

#include <cstdint>
#include <vector>

namespace {
    void processFunctionForInit(std::string& target, const char* target_orig, std::vector<CallParam_t *>& param_new, int num_params, CallParam_t* params) {
        for (int i = 0; i < num_params; i++) {
            param_new.push_back(&params[i]);
        }
        target = target_orig;
    }
}

ReleaseAnalysis::ReleaseAnalysis(void const* _func_supplier, ReleaseOp_t* rOP) {
    if (rOP->forbidden_op_kind == UNARY_READ || rOP->forbidden_op_kind == UNARY_WRITE) {
        forbIsRW = true;
        RWOp_t* rwOp = (RWOp_t*)rOP->forbidden_op;
        rwIdx = rwOp->idx;
        rwAcc = rwOp->accType;
    } else {
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

CallBacks ReleaseAnalysis::requiredCallbacksImpl() const {
    if (!forbIsRW) return {true, false, false};
    RWOp_t* rwOp = (RWOp_t*)forbiddenOp;
    return {true, !rwOp->isWrite, rwOp->isWrite};
}

Fulfillment ReleaseAnalysis::functionCBImpl(void* const& func, CallsiteInfo const& callsite) {
    // First, check if release
    for (void const* const& rel_func : rel_funcs) {
        if (rel_func == func) {
            if (params_release.empty()) {
                forbiddenCallsites.clear();
                forbMem.clear();
                return Fulfillment::UNKNOWN;
            }
            // Check which callsites are satisfied, remove from unchecked
            for (int i = 0; i < forbiddenCallsites.size();) {
                CallsiteInfo const& forbcallsite = forbiddenCallsites[i];
                if (DynamicUtils::checkFuncCallMatch(rel_func, params_release, callsite, forbcallsite, target_str_rel)) {
                    forbiddenCallsites.erase(forbiddenCallsites.begin() + i);
                    if (forbIsRW) forbMem.erase(forbMem.begin() + i);
                } else {
                    i++;
                }
            }
            // For the rest: Maybe actual fulfillment comes later
            return Fulfillment::UNKNOWN;
        }
    }

    // Check if forbidden
    for (void const* const& forb_func : forb_funcs) {
        if (forb_func == func) {
            if (params_forb.empty()) {
                for (CallsiteInfo const& forbCallsite : forbiddenCallsites) references.push_back(forbCallsite.location);
                references.push_back(callsite.location);
                return Fulfillment::VIOLATED;
            }

            // Check if a callsite is violated
            for (CallsiteInfo const& forbCallsite : forbiddenCallsites) {
                if (DynamicUtils::checkFuncCallMatch(forb_func, params_forb, callsite, forbCallsite, target_str_forb)) {
                    references.insert(references.end(), {forbCallsite.location, callsite.location});
                    return Fulfillment::VIOLATED;
                }
            }
        }
    }

    // Finally, check if supplier.
    // Needs to be done after check for forbidden, so that new supplier is not accidentally checked against itself
    if (func == func_supplier) {
        for (int i = 0; i < forbiddenCallsites.size(); i++) {
            if (forbiddenCallsites[i].location == callsite.location) {
                forbiddenCallsites[i] = callsite;
                if (forbIsRW) forbMem[i] = forbiddenCallsites[i].params[rwIdx];
                goto exit_rel_funccb;
            }
        }
        forbiddenCallsites.push_back(callsite);
        if (forbIsRW) forbMem.push_back(callsite.params[rwIdx]);
    }

    // Irrelevant function
    exit_rel_funccb:
    return Fulfillment::UNKNOWN;
}

Fulfillment ReleaseAnalysis::memoryCBImpl(CodePtr const& location, void const* const& memory, bool const& isWrite) {
    for (int i = 0; i < forbMem.size(); i++) {
        if (DynamicUtils::checkParamMatch(rwAcc, {&forbMem[rwIdx].value, sizeof(void*)*8}, {memory, sizeof(void*)*8})) {
            references.insert(references.end(), {forbiddenCallsites[i].location, location});
            return Fulfillment::VIOLATED;
        }
    }

    return Fulfillment::UNKNOWN;
}
