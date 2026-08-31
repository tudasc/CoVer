#include <algorithm>
#include <cstdint>
#include <vector>

class FuncCallsites {
    public:
        void addCallsite(std::vector<uintptr_t> const& params) {
            callsites.push_back(params);
        }
        bool checkMatch(int idx, uintptr_t val) const {
            return std::any_of(callsites.begin(), callsites.end(), [&](std::vector<uintptr_t> const& params){
                return params[idx] == val;
            });
        }
        operator bool() const { return !callsites.empty(); }
    private:
        std::vector<std::vector<uintptr_t>> callsites;
};
