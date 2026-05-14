#include "AllocAnalysis.h"
#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include "../DynamicUtils.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

int vecContains(std::vector<MemOpFunc_t> vec, void* f) {
    for (int i = 0; i < vec.size(); i++) {
        if (vec[i].func == f) return i;
    }
    return -1;
}

Fulfillment AllocAnalysis::functionPreCBImpl(void* const& func, CallsiteInfo const& callsite) {
    if (func == func_supplier) {
        uintptr_t ptr = (uintptr_t)callsite.params[idx].value;
        for (std::pair<uintptr_t, size_t> alloc_ptr : allocated) {
            if ((uintptr_t)alloc_ptr.first == ptr) return Fulfillment::UNKNOWN;
            if (ptr >= alloc_ptr.first && ptr < alloc_ptr.first + alloc_ptr.second) return Fulfillment::UNKNOWN;
        }
        references.push_back(callsite.location);
        return Fulfillment::VIOLATED;
    }
    return Fulfillment::UNKNOWN;
}

Fulfillment AllocAnalysis::functionPostCBImpl(void* const& func, CallsiteInfo const& callsite) {
    if (int idx = vecContains(mem_allocators, func); idx != -1) {
        MemOpFunc_t const& memop = mem_allocators[idx];
        uintptr_t alloc = (uintptr_t)(memop.rwOp->idx == 99 ? callsite.retval : callsite.params[memop.rwOp->idx].value);
        if (memop.rwOp->accType == ParamAccess::DEREF) alloc = (uintptr_t)*(void**)alloc;
        MathExpr_t const* cur = memop.size;
        size_t res = cur->isArgValue ? (size_t)callsite.params[cur->value].value : cur->value;
        while (cur->other != nullptr) {
            switch (cur->type) {
                case UNARY_VALUE:
                    break;
                case MULT:
                    res *= cur->other->isArgValue ? (size_t)callsite.params[cur->other->value].value : cur->other->value;
                    break;
            }
            cur = cur->other;
        }
        allocated.push_back({alloc, res});
    }
    else if (int idx = vecContains(mem_deallocators, func); idx != -1) {
        MemOpFunc_t const& memop = mem_deallocators[idx];
        uintptr_t const& target = (uintptr_t)(memop.rwOp->idx == 99 ? callsite.retval : callsite.params[memop.rwOp->idx].value);
        for (int i = 0; i < allocated.size(); i++) {
            if (target == allocated[i].first) {
                allocated.erase(allocated.begin() + i);
                break;
            }
        }
    }
    return Fulfillment::UNKNOWN;
}
