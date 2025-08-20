#pragma once

#include <list>
#include <string>

struct FileReference {
    std::string file;
    unsigned int line;
    unsigned int column;
};

struct ErrorMessage {
    std::string type;
    std::string error_id;
    std::string text;
    std::list<FileReference> references;
};
