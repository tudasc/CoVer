#include "ParamAnalysis.h"
#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include "../DynamicUtils.h"

#include <vector>

Fulfillment ParamAnalysis::functionCBImpl(void* const& func, CallsiteInfo const& callsite) {
    if (func != func_supplier) return Fulfillment::UNKNOWN;

    for (const ParamReq_t* req : param_requirements) {
        switch (req->comparator) {
            case Comparator::NEQ:
                if (callsite.params[idx].value != req->value) continue;
                return Fulfillment::VIOLATED;
            case Comparator::GTEQ:
                if (callsite.params[idx].value >= req->value) continue;
                return Fulfillment::VIOLATED;
            case Comparator::GT:
                if (callsite.params[idx].value >  req->value) continue;
                return Fulfillment::VIOLATED;
            case Comparator::LTEQ:
                if (callsite.params[idx].value <= req->value) continue;
                return Fulfillment::VIOLATED;
            case Comparator::LT:
                if (callsite.params[idx].value <  req->value) continue;
                return Fulfillment::VIOLATED;
            case Comparator::EXEQ:
                // EXEQ is the exception (pun), it overrides other forbidden values.
                if (callsite.params[idx].value == req->value) return Fulfillment::FULFILLED;
                continue;
        }
    }
    __builtin_unreachable();
}
