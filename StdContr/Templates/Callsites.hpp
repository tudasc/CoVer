#include <algorithm>
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

class FuncCallsites {
    public:
        struct Callsite {
            std::vector<uintptr_t> params;
            std::vector<int> tag_idx;
        };
        void addCallsite(Callsite const& callsite) {
            callsites.push_back(callsite);
        }
        bool checkMatchParam(int32_t idx, uintptr_t val, int8_t const& acc) const {
            return std::any_of(callsites.begin(), callsites.end(), [&](Callsite const& callsite){
                return primitiveMatch(val, callsite.params[idx], (ParamAccess)acc);
            });
        }
        bool checkMatchTag(uintptr_t val, int8_t const& acc) const {
            return std::any_of(callsites.begin(), callsites.end(), [&](Callsite const& callsite){
                for (int idx : callsite.tag_idx) {
                    if (primitiveMatch(val, callsite.params[idx], (ParamAccess)acc)) return true;
                }
                return false;
            });
        }
        operator bool() const { return !callsites.empty(); }
    private:
        std::vector<Callsite> callsites;
};
