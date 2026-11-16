#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {
    // Number must match those defined in ContractTree.hpp!
    enum ParamAccess { NORMAL = 0, DEREF = 1, ADDROF = 2 };
    inline bool primitiveMatch(uintptr_t const& contrP, uintptr_t const& callP, ParamAccess const& acc) {
        switch (acc) {
            case NORMAL: return contrP == callP;
            case DEREF: return (uintptr_t)*(void**)contrP == callP;
            case ADDROF: return contrP == (uintptr_t)*(void**)callP;
            default: std::unreachable();
        }
    }
}

// Calls recorded for one store
// N is the largest parameter count among functions recording into it
template <std::size_t N>
class FuncCallsites {
    public:
        struct Callsite {
            std::array<uintptr_t, N> params;
            // Set for the parameters carrying the tag this callsite was recorded for
            std::bitset<N> tag_params;
        };
        void addCallsite(std::array<uintptr_t, N> const& params, std::bitset<N> const& tag_params) {
            callsites.push_back({params, tag_params});
        }
        void eraseOnMatchParam(int32_t const& idx, uintptr_t const& val, int8_t const& acc) {
            std::erase_if(callsites, [&](Callsite const& callsite){
                return primitiveMatch(val, callsite.params[idx], (ParamAccess)acc);
            });
        }
        void eraseOnMatchContrParam(int32_t const& idx, uintptr_t const& val, int8_t const& acc) {
            std::erase_if(callsites, [&](Callsite const& callsite){
                return primitiveMatch(callsite.params[idx], val, (ParamAccess)acc);
            });
        }
        void eraseOnMatchTag(uintptr_t const& val, int8_t const& acc) {
            std::erase_if(callsites, [&](Callsite const& callsite){
                return tagMatch(callsite, val, (ParamAccess)acc);
            });
        }
        void clear() { callsites.clear(); }
        bool checkMatchParam(int32_t const& idx, uintptr_t const& val, int8_t const& acc) const {
            return std::any_of(callsites.begin(), callsites.end(), [&](Callsite const& callsite){
                return primitiveMatch(val, callsite.params[idx], (ParamAccess)acc);
            });
        }
        bool checkMatchTag(uintptr_t const& val, int8_t const& acc) const {
            return std::any_of(callsites.begin(), callsites.end(), [&](Callsite const& callsite){
                return tagMatch(callsite, val, (ParamAccess)acc);
            });
        }
        operator bool() const { return !callsites.empty(); }
    private:
        static bool tagMatch(Callsite const& callsite, uintptr_t const& val, ParamAccess const& acc) {
            for (unsigned long long mask = callsite.tag_params.to_ullong(); mask != 0; mask &= mask - 1) {
                if (primitiveMatch(val, callsite.params[std::countr_zero(mask)], acc)) return true;
            }
            return false;
        }
        std::vector<Callsite> callsites;
};
