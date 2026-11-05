export module ErrorMessage;
import std;

export struct FileReference {
    std::string file;
    unsigned int line;
    unsigned int column;
    bool operator==(FileReference const& other) const {
        return file == other.file && line == other.line && column == other.column;
    }
};

template<>
struct std::hash<FileReference> {
    std::size_t operator()(FileReference const& ref) const {
        return std::hash<unsigned int>()(ref.line) ^ std::hash<unsigned int>()(ref.column) ^ std::hash<std::string>()(ref.file);
    }
};

export struct ErrorMessage {
    std::string type;
    std::string error_id;
    std::string text;
    std::vector<FileReference> references;
};
