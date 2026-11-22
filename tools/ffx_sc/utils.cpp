// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "utils.h"

// Linux port (lyvra): wchar_t is UTF-32 on Linux; convert by hand instead of
// WideCharToMultiByte / MultiByteToWideChar.
std::string WCharToUTF8(const std::wstring& wstr)
{
    std::string out;
    out.reserve(wstr.size());
    for (wchar_t wc : wstr)
    {
        uint32_t c = static_cast<uint32_t>(wc);
        if (c < 0x80)
            out += static_cast<char>(c);
        else if (c < 0x800)
        {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::wstring UTF8ToWChar(const std::string& str)
{
    std::wstring out;
    out.reserve(str.size());
    for (size_t i = 0; i < str.size();)
    {
        unsigned char c = static_cast<unsigned char>(str[i]);
        uint32_t      cp;
        size_t        len;
        if (c < 0x80)              { cp = c;        len = 1; }
        else if ((c >> 5) == 0x6)  { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE)  { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
        else                       { cp = 0xFFFD;   len = 1; }
        for (size_t k = 1; k < len; ++k)
        {
            if (i + k >= str.size()) { cp = 0xFFFD; len = k; break; }
            cp = (cp << 6) | (static_cast<unsigned char>(str[i + k]) & 0x3F);
        }
        out += static_cast<wchar_t>(cp);
        i += len;
    }
    return out;
}
