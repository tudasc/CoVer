#pragma once

#include "BaseAnalysis.h"
#include "DynamicAnalysis.h"
#include <cstddef>
#include <cstdint>

struct AllocAnalysis : BaseAnalysis<AllocAnalysis> {
    public:
        AllocAnalysis(void const* _func_supplier, AllocOp_t* allocop) : idx(allocop->idx), acc(allocop->accType), func_supplier(_func_supplier) {
            for (int i = 0; i < allocop->num_allocators; i++) {
                mem_allocators.push_back(allocop->allocators[i]);
            }
            for (int i = 0; i < allocop->num_deallocators; i++) {
                mem_deallocators.push_back(allocop->deallocators[i]);
            }
        }

        ANALYSIS_PREAMBLE Fulfillment functionPreCBImpl(void* const& func, CallsiteInfo const& callsite);
        ANALYSIS_PREAMBLE Fulfillment functionPostCBImpl(void* const& func, CallsiteInfo const& callsite);
        ANALYSIS_PREAMBLE Fulfillment memoryCBImpl(CodePtr const& location, void const* const& memory, bool const& isWrite) const { return Fulfillment::UNKNOWN; }
        ANALYSIS_PREAMBLE Fulfillment exitCBImpl(CodePtr const& location) const { return Fulfillment::FULFILLED; }; // Evidently none of the callsites were erroneous 

        constexpr CallBacks requiredCallbacksImpl() const { return {true, true, false, false}; }

    private:
        // Configuration
        void const* func_supplier;
        int const idx;
        ParamAccess const acc;
        std::vector<std::pair<uintptr_t, size_t>> allocated;
        std::vector<MemOpFunc_t> mem_allocators;
        std::vector<MemOpFunc_t> mem_deallocators;
};
