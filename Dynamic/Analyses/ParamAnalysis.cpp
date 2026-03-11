#include "ParamAnalysis.h"
#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include "../DynamicUtils.h"

#include <vector>

Fulfillment ParamAnalysis::functionCBImpl(void* const& func, CallsiteInfo const& callsite) {
    if (func != func_supplier) return Fulfillment::UNKNOWN;

    for (const ParamReq_t* req : param_requirements) {
        const void* act_req = req->value;
        const void* act_callp = callsite.params[idx].value;
        if (req->need_deref) {
            act_req = (const void*)DynamicUtils::TruncateBits(*(int64_t*)act_req, callsite.params[idx].size);
            act_callp = (const void*)DynamicUtils::TruncateBits(*(int64_t*)act_callp, callsite.params[idx].size);
        }
        switch (req->comparator) {
            case Comparator::NEQ:
                if (act_callp != act_req) continue;
                return Fulfillment::VIOLATED;
            case Comparator::GTEQ:
                if (act_callp >= act_req) continue;
                return Fulfillment::VIOLATED;
            case Comparator::GT:
                if (act_callp >  act_req) continue;
                return Fulfillment::VIOLATED;
            case Comparator::LTEQ:
                if (act_callp <= act_req) continue;
                return Fulfillment::VIOLATED;
            case Comparator::LT:
                if (act_callp <  act_req) continue;
                return Fulfillment::VIOLATED;
            case Comparator::EXEQ:
                // EXEQ is the exception (pun), it overrides other forbidden values.
                if (act_callp == act_req) return Fulfillment::FULFILLED;
                continue;
        }
    }
    __builtin_unreachable();
}
