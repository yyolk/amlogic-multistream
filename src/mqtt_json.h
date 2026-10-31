#pragma once
#include "stream_manager.h"

#include <map>
#include <string>
#include <vector>

// Flat JSON object. Nested objects and arrays are ignored, not stored.
struct JsonObject {
    bool valid = false;
    std::map<std::string, std::string> strings;
    std::map<std::string, bool> bools;

    bool has(const std::string& key) const {
        return strings.find(key) != strings.end() || bools.find(key) != bools.end();
    }

    std::string getString(const std::string& key) const {
        auto it = strings.find(key);
        return it == strings.end() ? std::string() : it->second;
    }

    // Accepts a JSON bool, or the strings "true" and "false".
    bool getBool(const std::string& key, bool& out) const {
        auto b = bools.find(key);
        if (b != bools.end()) {
            out = b->second;
            return true;
        }
        auto s = strings.find(key);
        if (s == strings.end()) {
            return false;
        }
        if (s->second == "true") {
            out = true;
            return true;
        }
        if (s->second == "false") {
            out = false;
            return true;
        }
        return false;
    }
};

// Whitespace-only input is an empty object. Any other text must be one JSON object.
JsonObject parseFlatJsonObject(const std::string& text);

std::string jsonEscape(const std::string& value);

std::string buildStatusJson(bool online, bool playing, const std::string& layout,
                            const std::string& layoutSetting, const std::vector<StreamInfo>& streams);

std::string buildResponseJson(bool ok, const std::string& command, const std::string& id,
                              const std::string& message);
