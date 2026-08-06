module;

#include <string_view>
#include <string>

export module FunctionMods;

export namespace FunctionMods {
    struct ModBase {
        virtual ~ModBase() = default;
    };
    struct PreCallSentinel : ModBase {
        PreCallSentinel(std::string sen) : sentinel(sen) {}
        static std::string_view getID() { return "[Mode] PreCallSentinel"; }
        std::string sentinel;
    };
    struct PreCallCheck : ModBase {
        PreCallCheck(std::string sen) : sentinel(sen) {}
        static std::string_view getID() { return "[Mode] PreCallCheck"; }
        std::string sentinel;
    };
}
