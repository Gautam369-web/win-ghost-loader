#pragma once
#include <string>
#include <set>
#include <vector>
#include "third_party/json.hpp"
namespace sc {
using Json=nlohmann::json;
inline Json ParseJson(const std::string& text){
    bool valid=true;std::vector<std::set<std::string>> objects;
    auto callback=[&](int depth,Json::parse_event_t event,Json& value){
        if(depth>64)throw std::runtime_error("JSON nesting limit");
        if(event==Json::parse_event_t::object_start)objects.emplace_back();
        else if(event==Json::parse_event_t::key){if(objects.empty()||!objects.back().insert(value.get<std::string>()).second)valid=false;}
        else if(event==Json::parse_event_t::object_end)objects.pop_back();
        return true;
    };
    if(text.size()>4*1024*1024)return Json(Json::value_t::discarded);
    try{auto j=Json::parse(text,callback,false);return valid?j:Json(Json::value_t::discarded);}
    catch(...){return Json(Json::value_t::discarded);}
}
inline std::string JsonString(const std::string& text,const std::string& key){
    auto j=ParseJson(text);return j.is_object()&&j.contains(key)&&j[key].is_string()?j[key].get<std::string>():std::string{};
}
inline std::string JsonQuote(const std::string& text){return Json(text).dump();}
inline std::string UserConfig(const std::string& name,const std::string& reg,const std::string& subject,const std::string& key){
    return Json{{"name",name},{"registration_number",reg},{"default_language",subject},{"personal_api_key",key}}.dump(2)+"\n";
}
inline std::string GeminiText(const std::string& text){
    auto j=ParseJson(text);
    if(!j.is_object()||!j.contains("candidates")||!j["candidates"].is_array()||j["candidates"].empty())return {};
    const auto& c=j["candidates"][0];
    if(!c.is_object()||!c.contains("finishReason")||c["finishReason"]!="STOP"||!c.contains("content")||!c["content"].is_object())return {};
    const auto& content=c["content"];
    if(!content.contains("parts")||!content["parts"].is_array())return {};
    std::string out;
    for(const auto& p:content["parts"]){
        if(!p.is_object())return {};
        if(p.contains("thought")&&p["thought"]==true)continue;
        if(p.contains("text")){if(!p["text"].is_string())return {};out+=p["text"].get<std::string>();}
    }
    return out;
}
inline char McqLetter(const std::string& text){
    size_t a=text.find_first_not_of(" \t\r\n"),b=text.find_last_not_of(" \t\r\n");
    if(a==std::string::npos||a!=b)return 0;
    char c=text[a];if(c>='a'&&c<='d')c-=32;return c>='A'&&c<='D'?c:0;
}
}
