#pragma once

#include <list>
#include <string>

struct ErrorReference {
    std::string file;
    unsigned int line;
    unsigned int column;
};

struct ErrorMessage {
    std::string type;
    std::string error_id;
    std::string text;
    std::list<ErrorReference> references;
};
