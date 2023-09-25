// Copyright 2021-2022 Software Quality Laboratory, NYCU.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef S2E_PLUGINS_CRAX_STRING_UTIL_H
#define S2E_PLUGINS_CRAX_STRING_UTIL_H

#include <cstring>
#include <iterator>
#include <memory>
#include <istream>
#include <sstream>
#include <string>
#include <vector>
#include <type_traits>

namespace s2e::plugins::crax {

std::vector<std::string> split(const std::string &s, const char delim);
std::vector<std::string> split(const std::string &s, const std::string &delim);

std::string join(const std::vector<std::string> &strings, const std::string &delim);
std::string replace(std::string s, const std::string &keyword, const std::string &newword);
std::string slice(std::string s, size_t start, size_t end = std::string::npos);  // [start, end)
std::string strip(std::string s);
std::string ljust(std::string s, size_t size, char c);

bool startsWith(const std::string &s, const std::string &prefix);
bool endsWith(const std::string &s, const std::string &suffix);
bool isNumString(const std::string &s);


template <typename... Args>
std::string format(const std::string &fmt, Args &&...args) {
  // std::snprintf(dest, n, fmt, ...) returns the number of chars
  // that will be actually written into `dest` if `n` is large enough,
  // not counting the terminating null character.
  const int bufSize
    = 1 + std::snprintf(nullptr, 0, fmt.c_str(), std::forward<Args>(args)...);

  const auto buf = std::make_unique<char[]>(bufSize);
  std::memset(buf.get(), 0x00, bufSize);

  return (std::snprintf(buf.get(), bufSize, fmt.c_str(),
          std::forward<Args>(args)...) > 0) ? std::string(buf.get()) : "";
}

// Given a sequence of bytes, convert them to python3 byte strings.
template <typename InputIt>
std::string toByteString(InputIt first, InputIt last) {
    std::string ret = "b'";
    uint8_t byte = 0;
    size_t combo = 0;
    bool isPrevStringClosed = false;

    for (auto it = first; it != last; it++) {
        byte = *it;
        combo = 1;

        while (std::next(it) != last && *it == *std::next(it)) {
            combo++;
            it++;
        }

        if (combo == 1) {
            ret += format("\\x%02x", byte);
            isPrevStringClosed = false;
        } else {
            if (!isPrevStringClosed && ret.size() > 2) {
                ret += "' + b'";
            }
            ret += format("\\x%02x' * %d", byte, combo);
            isPrevStringClosed = true;
            if (std::next(it) != last) {
                ret += " + b'";
                isPrevStringClosed = false;
            }
        }
    }

    if (!isPrevStringClosed) {
        ret += '\'';
    }

    return '(' + ret + ')';
}

template <typename T>
std::string streamToString(const T &s) {
    std::stringstream ss;
    ss << s.rdbuf();
    return ss.str();
}

template <typename InputIt, typename ElementToString>
std::string toString(InputIt first,
                     InputIt last,
                     char left,
                     char right,
                     ElementToString f) {
    std::string ret;
    ret += left;
    for (auto it = first; it != last; it++) {
        ret += f(it);
        if (std::next(it) != last) {
            ret += ", ";
        }
    }
    ret += right;
    return ret;
}

}  // namespace s2e::plugins::crax

#endif  // S2E_PLUGINS_CRAX_STRING_UTIL_H
