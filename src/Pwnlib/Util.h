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

#ifndef S2E_PLUGINS_CRAX_PWNLIB_UTIL_H
#define S2E_PLUGINS_CRAX_PWNLIB_UTIL_H

#include <cstdint>
#include <vector>

namespace s2e::plugins::crax {

// Converts an uint64_t to a vector of bytes in little endian.
std::vector<uint8_t> p64(uint64_t val);

// Converts a sequence of bytes in little endian to uint64_t.
uint64_t u64(const std::vector<uint8_t> &bytes);

// This is to avoid "constant expression evaluates to -xx which
// cannot be narrowed to type 'char' [-Wc++11-narrowing] error".
inline constexpr uint8_t u8(uint8_t v) {
    return static_cast<uint8_t>(v);
}

}  // namespace s2e::plugins::crax

#endif  // S2E_PLUGINS_CRAX_PWNLIB_UTIL_H
