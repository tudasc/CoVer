#pragma once

#include <list>
#include <string>

struct FileReference {
    std::string file;
    unsigned int line;
    unsigned int column;

    bool operator<(const FileReference other) const {
        return file < other.file && line < other.line && column < other.column;
    }
};

struct ErrorMessage {
    std::string type;
    std::string error_id;
    std::string text;
    std::list<FileReference> references;
};
