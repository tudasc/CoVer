#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <cstddef>
#include <cstdint>
#include <unordered_map>

struct AllocAnalysis : BaseAnalysis<AllocAnalysis> {
    public:
        AllocAnalysis(void const* _func_supplier, AllocOp_t* allocop) : idx(allocop->idx), acc(allocop->accType), func_supplier(_func_supplier) {
            for (int i = 0; i < allocop->num_allocators; i++) {
                mem_allocators[allocop->allocators[i].func] = &allocop->allocators[i];
            }
            for (int i = 0; i < allocop->num_deallocators; i++) {
                mem_deallocators[allocop->deallocators[i].func] = &allocop->deallocators[i];
            }
        }

        ANALYSIS_PREAMBLE Fulfillment functionCBImpl(void* const& func, bool const isPre, CallsiteInfo const& callsite);
        ANALYSIS_PREAMBLE Fulfillment memoryCBImpl(CodePtr const& location, void const* const& memory, bool const& isWrite) const { return Fulfillment::UNKNOWN; }
        ANALYSIS_PREAMBLE Fulfillment exitCBImpl(CodePtr const& location) const { return Fulfillment::FULFILLED; }; // Evidently none of the callsites were erroneous 

        constexpr CallBacks requiredCallbacksImpl() const { return {true, true, false, false}; }

    private:
        // Configuration
        void const* func_supplier;
        int const idx;
        ParamAccess const acc;
        std::unordered_map<uintptr_t, size_t> allocated;
        std::unordered_map<void const*, MemOpFunc_t const*> mem_allocators;
        std::unordered_map<void const*, MemOpFunc_t const*> mem_deallocators;
};
