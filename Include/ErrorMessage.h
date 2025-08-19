#pragma once

#include <functional>
#include <list>
#include <string>

struct FileReference {
    std::string file;
    unsigned int line;
    unsigned int column;
    bool operator==(FileReference const& other) const {
        return file == other.file && line == other.line && column == other.column;
    }
};

template<>
struct std::hash<FileReference> {
    size_t operator()(FileReference const& ref) const {
        return std::hash<uint>()(ref.line) ^ std::hash<uint>()(ref.column) ^ std::hash<std::string>()(ref.file);
    }
};

struct ErrorMessage {
    std::string type;
    std::string error_id;
    std::string text;
    std::list<FileReference> references;
};
