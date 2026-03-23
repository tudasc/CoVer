#include "AllocAnalysis.h"
#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include "../DynamicUtils.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

Fulfillment AllocAnalysis::functionCBImpl(void* const& func, bool const isPre, CallsiteInfo const& callsite) {
    if (!isPre) {
        if (mem_allocators.contains(func)) {
            uintptr_t alloc = (uintptr_t)(mem_allocators[func]->rwOp->idx == 99 ? callsite.retval : callsite.params[mem_allocators[func]->rwOp->idx].value);
            if (mem_allocators[func]->rwOp->accType == ParamAccess::DEREF) alloc = (uintptr_t)*(void**)alloc;
            MathExpr_t const* cur = mem_allocators[func]->size;
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
            allocated[alloc] = res;
        }
        else if (mem_deallocators.contains(func)) {
            if (mem_deallocators[func]->rwOp->idx == 99) allocated.erase((uintptr_t const)callsite.retval);
            else allocated.erase((uintptr_t const)callsite.params[mem_deallocators[func]->rwOp->idx].value);
        }
        return Fulfillment::UNKNOWN;
    } else {
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
}
