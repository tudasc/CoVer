#include "ParamAnalysis.h"
#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include "../DynamicUtils.h"

#include <cstdint>
#include <vector>

static constexpr int64_t sign_extend(uintptr_t const ptr, int const size) {
    switch (size) {
        case 8: return (int64_t)(int8_t)ptr; break;
        case 16: return (int64_t)(int16_t)ptr; break;
        case 32: return (int64_t)(int32_t)ptr; break;
    }
    return ptr;
}

Fulfillment ParamAnalysis::functionCBImpl(void* const& func, bool const isPre, CallsiteInfo const& callsite) {
    if (func != func_supplier) return Fulfillment::UNKNOWN;

    void const* act_callp = callsite.params[idx].value;
    if (callval_need_deref) {
        act_callp = (const void*)(*(void**)act_callp);
    }
    int64_t const int_callp = sign_extend((uintptr_t)act_callp, callsite.params[idx].size);
    for (const ParamReq_t* req : param_requirements) {
        void const* act_req = req->isArg ? callsite.params[(int64_t)req->value].value: req->value;
        if (req->reqval_need_deref) {
            act_req = (const void*)*(void**)act_req;
        }
        int64_t const int_req = sign_extend((uintptr_t)act_req, callsite.params[idx].size);
        switch (req->comparator) {
            case Comparator::EXEQ:
                // EXEQ is the exception (pun), it overrides other forbidden values.
                if (act_callp == act_req || int_callp == int_req) return Fulfillment::FULFILLED;
                continue;
            case Comparator::EQ:
                if (act_callp == act_req && int_callp == int_req) continue;
                references.push_back(callsite.location); return Fulfillment::VIOLATED;
            case Comparator::NEQ:
                if (act_callp != act_req && int_callp != int_req) continue;
                references.push_back(callsite.location); return Fulfillment::VIOLATED;
            // Smaller/Larger comparisons dont make sense for pointers. Assume ints from here
            case Comparator::GTEQ:
                if (int_callp >= int_req) continue;
                references.push_back(callsite.location); return Fulfillment::VIOLATED;
            case Comparator::GT:
                if (int_callp >  int_req) continue;
                references.push_back(callsite.location); return Fulfillment::VIOLATED;
            case Comparator::LTEQ:
                if (int_callp <= int_req) continue;
                references.push_back(callsite.location); return Fulfillment::VIOLATED;
            case Comparator::LT:
                if (int_callp <  int_req) continue;
                references.push_back(callsite.location); return Fulfillment::VIOLATED;
        }
    }
    return Fulfillment::UNKNOWN;
}
