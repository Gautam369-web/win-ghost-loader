#pragma once
#include <algorithm>
#include "JsonSupport.h"
namespace sc {
// Precision touchpads send fractions of a 120-unit mouse-wheel notch.
// Preserve sub-pixel motion instead of rounding each event down to zero.
struct WheelPixels {
    long long remainder=0;
    int consume(int delta){
        const long long scaled=remainder+static_cast<long long>(delta)*20;
        remainder=scaled%120;
        return static_cast<int>(scaled/120);
    }
};
inline int ScrollOffset(int offset,int step,int maximum){
    const long long next=static_cast<long long>(offset)-step;
    return static_cast<int>(std::max(0LL,std::min(next,static_cast<long long>(std::max(0,maximum)))));
}
inline std::string CleanResponse(const std::string& raw) {
    // Only remove a complete outer Markdown fence. Never dedent the content.
    size_t first=raw.find_first_not_of(" \t\r\n");
    size_t last=raw.find_last_not_of(" \t\r\n");
    if (first==std::string::npos || raw.compare(first,3,"```")!=0) return raw;
    size_t openingEnd=raw.find('\n',first);
    if (openingEnd==std::string::npos || last<2 || raw.compare(last-2,3,"```")!=0) return raw;
    size_t closing=last-2;
    size_t closingLine=raw.rfind('\n',closing);
    if (closingLine==std::string::npos || closingLine<openingEnd) return raw;
    if (raw.find_first_not_of(" \t\r",closingLine+1)!=closing) return raw;
    if(raw.find("```",openingEnd+1)!=closing)return {}; size_t end=closingLine;
    if (end>openingEnd && raw[end-1]=='\r') --end;
    if (end<=openingEnd) return {};
    return raw.substr(openingEnd+1,end-openingEnd-1);
}

inline std::wstring NormalizeNewlines(const std::wstring& value) {
    std::wstring out;
    for (size_t i=0; i<value.size(); ++i) {
        if (value[i]==L'\r') {
            out+=L'\n';
            if (i+1<value.size() && value[i+1]==L'\n') ++i;
        } else out+=value[i];
    }
    return out;
}
}
