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
inline std::string StripModeLabel(const std::string& s) {
    // Remove any "MODE A", "MODE B", "Mode A:", "Mode B —" etc. line that
    // the AI echoes back at the very start of its output.
    // Pattern: optional whitespace, "mode" (case-insensitive), space, letter, optional punctuation/text, newline
    std::string out = s;
    while (true) {
        size_t p = out.find_first_not_of(" \t\r\n");
        if (p == std::string::npos) break;
        // Check for case-insensitive "MODE " or "Mode "
        if (p + 5 >= out.size()) break;
        char c0=out[p], c1=out[p+1], c2=out[p+2], c3=out[p+3], c4=out[p+4];
        bool isMode = (c0=='M'||c0=='m') && (c1=='O'||c1=='o') && (c2=='D'||c2=='d') && (c3=='E'||c3=='e') && c4==' ';
        if (!isMode) break;
        // Skip to end of this line
        size_t nl = out.find('\n', p);
        if (nl == std::string::npos) { out.clear(); break; }
        out = out.substr(nl + 1);
    }
    return out;
}

inline std::string CleanResponse(const std::string& raw) {
    // Strip any leading MODE A / MODE B echo from the AI
    std::string s = StripModeLabel(raw);
    // Only remove a complete outer Markdown fence. Never dedent the content.
    size_t first=s.find_first_not_of(" \t\r\n");
    size_t last=s.find_last_not_of(" \t\r\n");
    if (first==std::string::npos || s.compare(first,3,"```")!=0) return s;
    size_t openingEnd=s.find('\n',first);
    if (openingEnd==std::string::npos || last<2 || s.compare(last-2,3,"```")!=0) return s;
    size_t closing=last-2;
    size_t closingLine=s.rfind('\n',closing);
    if (closingLine==std::string::npos || closingLine<openingEnd) return s;
    if (s.find_first_not_of(" \t\r",closingLine+1)!=closing) return s;
    if(s.find("```",openingEnd+1)!=closing)return {}; size_t end=closingLine;
    if (end>openingEnd && s[end-1]=='\r') --end;
    if (end<=openingEnd) return {};
    return s.substr(openingEnd+1,end-openingEnd-1);
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
