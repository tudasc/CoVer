export module AnnotDocs;
import std;

// The annotation documentation in AnnotDocs/, embedded into the binary for the language model tools
namespace AnnotDocs {

constexpr unsigned char ALIAS[] = {
#embed "../AnnotDocs/AliasAnnotations.md"
};
constexpr unsigned char ALLOCATION[] = {
#embed "../AnnotDocs/AllocationAnnotations.md"
};
constexpr unsigned char FUNCTION_POINTER[] = {
#embed "../AnnotDocs/FunctionPointerAnnotations.md"
};

template<std::size_t N>
std::string_view Text(unsigned char const (&data)[N]) {
    return std::string_view(reinterpret_cast<char const*>(data), N);
}

export struct Doc {
    std::string topic;
    std::string_view text;
};

// All documents, in a fixed order
export std::vector<Doc> const& Docs() {
    static std::vector<Doc> const docs = {
        {"alias", Text(ALIAS)},
        {"allocation", Text(ALLOCATION)},
        {"function_pointer", Text(FUNCTION_POINTER)},
    };
    return docs;
}

} // namespace AnnotDocs
