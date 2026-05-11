#include "AnnotVerifier.h"
#include "Contracts.h"
#include <cstdarg>
#include <iostream>
#include <vector>

std::vector<std::pair<void const*, void const*>> fp_targets;
extern "C" void __attribute__((visibility("default"))) CoVer_AnnotFP(void const* ptr, int num_targets, ...) {
    va_list list;
    va_start(list, num_targets);
    for (int i = 0; i < num_targets; i++) {
        void* target = va_arg(list, void*);
        if (target == ptr) return;
    }
    std::cerr << "CoVer-Dynamic: Annotation verification failed! (Funcptr)\n";
    va_end(list);
}

struct AliasGroupAbstr {
    int idx;
    void* cmp;
};
std::vector<AliasGroupAbstr> aliasinfo;
extern "C" void __attribute__((visibility("default"))) CoVer_AnnotAlias(void* ptr, bool shouldAlias, int group) {
    for (AliasGroupAbstr const& Agroup : aliasinfo) {
        if (Agroup.idx == group) {
            if (shouldAlias && ptr == Agroup.cmp) return;
            if (!shouldAlias && ptr != Agroup.cmp) return;
            std::cerr << "CoVer-Dynamic: Annotation verification failed! (Alias)\n";
            return;
        }
    }
    aliasinfo.push_back({group, ptr});
}
