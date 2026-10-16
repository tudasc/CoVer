#include "AnnotVerifier.h"
#include <cstdarg>
#include <cstdint>
#include <dlfcn.h>
#include <format>
#include <iostream>
#include <vector>
#include <array>
#include <optional>
#include <algorithm>
#include <setjmp.h>
#include <csignal>
#include <mutex>
#include <unistd.h>

namespace {
    std::string exec(std::string const& cmd) {
        std::array<char, 128> buffer;
        std::string result;
        FILE* pipe = popen(cmd.c_str(), "r");
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            result += buffer.data();
        }
        int ret = WEXITSTATUS(pclose(pipe));
        if (ret != 0) {
            exit(ret);
        }
        return result;
    }

    std::ostream& AnnotErr() { return std::cerr << "CoVer-AnnotVerifier: "; }
}

namespace {
    std::string getFileRefStr(std::string file, void const* parsed_loc) {
#ifdef CMAKE_ADDR2LINE
        constexpr int SCAN_RANGE = 64;
        for (int i = 0; i <= SCAN_RANGE; i++) {
            std::string scan_cmd = std::format("{} -e {} {:x}", CMAKE_ADDR2LINE, file, (uintptr_t)parsed_loc - i);
            std::string scan_results = exec(scan_cmd);
            scan_results.pop_back();
            if (!scan_results.empty() && !scan_results.ends_with(":?") && !scan_results.ends_with(":0")) {
                return scan_results;
            }
        }
        return "";
#else
        return std::format("{}[0x{:x}] (Cannot resolve: No addr2line support configured)\n", file, (uintptr_t)parsed_loc);
#endif
    }

    std::optional<std::pair<std::string, void const*>> getDLInfo(void const* location) {
        Dl_info info;
        if (!dladdr(location, &info)) {
            return std::nullopt;
        }
        if ((intptr_t)info.dli_fbase != (intptr_t)0x400000) {
            // if the filebase is not 0x400000, we have an VMA offset that we have to subtract
            // otherwise, it is a PIE so we can just use the codePtr
            // Additionally, subtract 1 to get from return address to call
            location = (void*)((intptr_t)location - (intptr_t)info.dli_fbase - 1);
        }
        return std::optional<std::pair<std::string, void const*>>({std::string(info.dli_fname), location});
    }
    std::string getFileRefStr(void const* location) {
        std::optional<std::pair<std::string, const void *>> dlinfo = getDLInfo(location);
        if (!dlinfo) {
            return "dladdr failure!";
        }
        return getFileRefStr(dlinfo->first, dlinfo->second);
    }

    std::string getFuncName(void const* ptr) {
        Dl_info info;
#ifdef CMAKE_ADDR2LINE
        if (!dladdr(ptr, &info)) return "?";
        uintptr_t offset = (uintptr_t)ptr;
        if ((intptr_t)info.dli_fbase != (intptr_t)0x400000)
            offset -= (uintptr_t)info.dli_fbase;
        std::string out = exec(std::format("{} -f -e {} {:x}", CMAKE_ADDR2LINE, info.dli_fname, offset));
        size_t nl = out.find('\n');
        if (nl != std::string::npos) out.resize(nl);
        if (out.empty()) return "??";
        return out;
#else
        return "?? (No addr2line support configured)";
#endif
    }
}


std::vector<std::pair<void const*, void const*>> fp_targets;
extern "C" void __attribute__((visibility("default"))) CoVer_AnnotFP(void const* ptr, int num_targets, ...) {
    va_list list;
    va_start(list, num_targets);
    for (int i = 0; i < num_targets; i++) {
        void* target = va_arg(list, void*);
        if (target == ptr) { va_end(list); return; }
    }
    AnnotErr() << "Annotation verification failed!\n";
    AnnotErr() << "Function pointer call at " << getFileRefStr(__builtin_return_address(0)) << " targets " << getFuncName(ptr) << ", which is not in the annotation allowlist!\n";
    va_end(list);
}

struct AliasGroupAbstr {
    int idx;
    void* cmp;
    void* prev_loc;
};
std::vector<AliasGroupAbstr> aliasinfo;
extern "C" void __attribute__((visibility("default"))) CoVer_AnnotAlias(void* ptr, bool shouldAlias, int group) {
    for (AliasGroupAbstr const& Agroup : aliasinfo) {
        if (Agroup.idx == group) {
            if (shouldAlias && ptr == Agroup.cmp) return;
            if (!shouldAlias && ptr != Agroup.cmp) return;
            AnnotErr() << "Annotation verification failed!\n";
            AnnotErr() << "Alias group " << group << " has two values which " << (shouldAlias ? "do not alias" : "alias") << " even though they should" << (shouldAlias ? "" : " not") << "\n";
            AnnotErr() << "First value found at " << getFileRefStr(Agroup.prev_loc) << "\n";
            AnnotErr() << "Second value found at " << getFileRefStr(__builtin_return_address(0)) << "\n";
            return;
        }
    }
    aliasinfo.push_back({group, ptr, __builtin_return_address(0)});
}
extern "C" void __attribute__((visibility("default"))) CoVer_AnnotAliasEpoch(int group) {
    std::erase_if(aliasinfo, [group](AliasGroupAbstr const& Agroup) { return Agroup.idx == group; });
}

namespace {
    std::mutex probe_mutex;
    struct sigaction prev_segv, prev_bus;
    thread_local sigjmp_buf probe_env;
    thread_local volatile sig_atomic_t probing = 0;

    void probeHandler(int, siginfo_t*, void*) {
        if (probing) siglongjmp(probe_env, 1);
        // Fault in another thread: Restore previous handlers, faulting instruction is re-executed on return
        sigaction(SIGSEGV, &prev_segv, nullptr);
        sigaction(SIGBUS, &prev_bus, nullptr);
    }

    // Dereferences ptr and each page start up to ptr+size, catching segfaults.
    // Returns number of bytes accessible from ptr (page granularity, at most size). size 0 only checks ptr itself.
    size_t probeAccessible(void const* ptr, size_t size) {
        std::lock_guard lock(probe_mutex);
        struct sigaction sa = {};
        sa.sa_sigaction = probeHandler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &prev_segv);
        sigaction(SIGBUS, &sa, &prev_bus);

        uintptr_t const page_size = sysconf(_SC_PAGESIZE);
        uintptr_t const begin = (uintptr_t)ptr;
        uintptr_t const end = begin + std::min<size_t>(std::max<size_t>(size, 1), UINTPTR_MAX - begin);
        volatile uintptr_t cur = begin;
        size_t res;
        probing = 1;
        if (sigsetjmp(probe_env, 1) == 0) {
            while (true) {
                (void)*(char const volatile*)cur;
                uintptr_t next = (cur & ~(page_size - 1)) + page_size;
                if (next >= end || next < cur) break;
                cur = next;
            }
            res = end - begin;
        } else {
            res = cur - begin;
        }
        probing = 0;

        sigaction(SIGSEGV, &prev_segv, nullptr);
        sigaction(SIGBUS, &prev_bus, nullptr);
        return res;
    }
}

extern "C" void __attribute__((visibility("default"))) CoVer_AnnotAlloc(void* ptr, size_t size) {
    size_t accessible = probeAccessible(ptr, size);
    if (accessible == 0) {
        AnnotErr() << "Annotation verification failed!\n";
        AnnotErr() << "Allocation annotation at " << getFileRefStr(__builtin_return_address(0)) << " references pointer " << ptr << ", which cannot be dereferenced!\n";
        return;
    }
    if (size != 0 && accessible < size) {
        AnnotErr() << "Annotation verification failed!\n";
        AnnotErr() << "Allocation annotation at " << getFileRefStr(__builtin_return_address(0)) << " annotates pointer " << ptr << " with " << size << " bytes, but only " << accessible << " bytes are allocated!\n";
    }
}
