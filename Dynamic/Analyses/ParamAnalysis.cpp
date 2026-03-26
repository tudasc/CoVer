#include "ParamAnalysis.h"
#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include "../DynamicUtils.h"

#include <cstdint>
#include <vector>

Fulfillment ParamAnalysis::functionCBImpl(void* const& func, bool const isPre, CallsiteInfo const& callsite) {
    if (func != func_supplier) return Fulfillment::UNKNOWN;

    for (const ParamReq_t* req : param_requirements) {
        const void* act_req = req->isArg ? callsite.params[(int64_t)req->value].value: req->value;
        const void* act_callp = callsite.params[idx].value;
        if (req->need_deref) {
            act_req = (const void*)DynamicUtils::TruncateBits(*(int64_t*)act_req, callsite.params[idx].size);
            act_callp = (const void*)DynamicUtils::TruncateBits(*(int64_t*)act_callp, callsite.params[idx].size);
        } else {
            act_callp = (const void*)DynamicUtils::TruncateBits((uintptr_t)act_callp, callsite.params[idx].size);
        }
        switch (req->comparator) {
            case Comparator::EXEQ:
                // EXEQ is the exception (pun), it overrides other forbidden values.
                if (act_callp == act_req) return Fulfillment::FULFILLED;
                continue;
            case Comparator::NEQ:
                if (act_callp != act_req) continue;
                references.push_back(callsite.location); return Fulfillment::VIOLATED;
            // Smaller/Larger comparisons dont make sense for pointers. Assume ints from here
            default:
                switch (callsite.params[idx].size) {
                    case 8: act_callp = (void*)(int64_t)(int8_t)(uintptr_t)act_callp; break;
                    case 16: act_callp = (void*)(int64_t)(int16_t)(uintptr_t)act_callp; break;
                    case 32: act_callp = (void*)(int64_t)(int32_t)(uintptr_t)act_callp; break;
                }
                switch (req->comparator) {
                    case Comparator::GTEQ:
                        if ((int64_t)act_callp >= (int64_t)act_req) continue;
                        references.push_back(callsite.location); return Fulfillment::VIOLATED;
                    case Comparator::GT:
                        if ((int64_t)act_callp >  (int64_t)act_req) continue;
                        references.push_back(callsite.location); return Fulfillment::VIOLATED;
                    case Comparator::LTEQ:
                        if ((int64_t)act_callp <= (int64_t)act_req) continue;
                        references.push_back(callsite.location); return Fulfillment::VIOLATED;
                    case Comparator::LT:
                        if ((int64_t)act_callp <  (int64_t)act_req) continue;
                        references.push_back(callsite.location); return Fulfillment::VIOLATED;
                    case Comparator::EXEQ:
                    case Comparator::NEQ:
                        __builtin_unreachable();
                }
        }
    }
    return Fulfillment::UNKNOWN;
}
