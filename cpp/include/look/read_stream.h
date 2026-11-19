#pragma once
// Reads the rest of a stream into a string in one go (a bulk copy from the filebuf).
//
// reading with istreambuf_iterator took the file character by character and grew the string many times:
// measured, 7% of the commands of a qr-menu CGI request (the source + included files). The stream's
// mode is preserved exactly — on a file opened in text mode Windows' \r\n → \n translation is as before.
#include <istream>
#include <sstream>
#include <string>

namespace look {

inline std::string read_stream(std::istream& in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace look
