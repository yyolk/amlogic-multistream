#include "mqtt_json.h"

#include <cctype>
#include <string>

namespace {

void appendUtf8(std::string& out, int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool parse(JsonObject& out) {
        skipWs();
        if (i_ >= s_.size()) {
            out.valid = true;
            return true;
        }
        if (!parseObject(&out)) {
            return false;
        }
        skipWs();
        if (i_ != s_.size()) {
            return false;
        }
        out.valid = true;
        return true;
    }

private:
    static constexpr int kMaxDepth = 32;

    const std::string& s_;
    size_t i_ = 0;
    int depth_ = 0;

    void skipWs() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) {
            ++i_;
        }
    }

    bool eat(char c) {
        skipWs();
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    bool matchWord(const char* literal) {
        const size_t n = std::char_traits<char>::length(literal);
        if (s_.compare(i_, n, literal) != 0) {
            return false;
        }
        i_ += n;
        return true;
    }

    bool parseHex4(int& cp) {
        if (i_ + 4 > s_.size()) {
            return false;
        }
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const unsigned char h = static_cast<unsigned char>(s_[i_++]);
            cp <<= 4;
            if (h >= '0' && h <= '9') {
                cp += h - '0';
            } else if (h >= 'a' && h <= 'f') {
                cp += h - 'a' + 10;
            } else if (h >= 'A' && h <= 'F') {
                cp += h - 'A' + 10;
            } else {
                return false;
            }
        }
        return true;
    }

    bool parseString(std::string& out) {
        skipWs();
        if (i_ >= s_.size() || s_[i_] != '"') {
            return false;
        }
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= s_.size()) {
                return false;
            }
            const char e = s_[i_++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                out.push_back(e);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                int cp = 0;
                if (!parseHex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) {
                    return false;
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool parseNumber(std::string* literal) {
        const size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') {
            ++i_;
        }
        if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) {
            return false;
        }
        if (s_[i_] == '0') {
            ++i_;
        } else {
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
                ++i_;
            }
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) {
                return false;
            }
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
                ++i_;
            }
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) {
                ++i_;
            }
            if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) {
                return false;
            }
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
                ++i_;
            }
        }
        if (literal) {
            *literal = s_.substr(start, i_ - start);
        }
        return true;
    }

    bool parseObject(JsonObject* store);
    bool skipArray();
    bool skipValue();
    bool parseValueStore(JsonObject& obj, const std::string& key);

    bool enter() {
        if (depth_ >= kMaxDepth) {
            return false;
        }
        ++depth_;
        return true;
    }

    void leave() { --depth_; }
};

bool Parser::skipArray() {
    if (!enter()) {
        return false;
    }
    if (!eat('[')) {
        return false;
    }
    if (eat(']')) {
        leave();
        return true;
    }
    while (true) {
        if (!skipValue()) {
            return false;
        }
        if (eat(']')) {
            leave();
            return true;
        }
        if (!eat(',')) {
            return false;
        }
    }
}

bool Parser::skipValue() {
    skipWs();
    if (i_ >= s_.size()) {
        return false;
    }
    const char c = s_[i_];
    if (c == '"') {
        std::string ignored;
        return parseString(ignored);
    }
    if (c == '{') {
        return parseObject(nullptr);
    }
    if (c == '[') {
        return skipArray();
    }
    if (c == 't') {
        return matchWord("true");
    }
    if (c == 'f') {
        return matchWord("false");
    }
    if (c == 'n') {
        return matchWord("null");
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
        return parseNumber(nullptr);
    }
    return false;
}

bool Parser::parseValueStore(JsonObject& obj, const std::string& key) {
    skipWs();
    if (i_ >= s_.size()) {
        return false;
    }
    const char c = s_[i_];
    if (c == '"') {
        std::string value;
        if (!parseString(value)) {
            return false;
        }
        obj.bools.erase(key);
        obj.strings[key] = std::move(value);
        return true;
    }
    if (c == 't' || c == 'f') {
        const bool value = c == 't';
        if (!matchWord(value ? "true" : "false")) {
            return false;
        }
        obj.strings.erase(key);
        obj.bools[key] = value;
        return true;
    }
    if (c == 'n') {
        if (!matchWord("null")) {
            return false;
        }
        obj.strings.erase(key);
        obj.bools.erase(key);
        return true;
    }
    if (c == '{' || c == '[') {
        return skipValue();
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
        std::string literal;
        if (!parseNumber(&literal)) {
            return false;
        }
        obj.bools.erase(key);
        obj.strings[key] = std::move(literal);
        return true;
    }
    return false;
}

bool Parser::parseObject(JsonObject* store) {
    if (!enter()) {
        return false;
    }
    if (!eat('{')) {
        return false;
    }
    if (eat('}')) {
        leave();
        return true;
    }
    while (true) {
        std::string key;
        if (!parseString(key)) {
            return false;
        }
        if (!eat(':')) {
            return false;
        }
        if (store) {
            if (!parseValueStore(*store, key)) {
                return false;
            }
        } else if (!skipValue()) {
            return false;
        }
        if (eat('}')) {
            leave();
            return true;
        }
        if (!eat(',')) {
            return false;
        }
    }
}

void appendEscaped(std::string& out, const std::string& value) {
    out.push_back('"');
    for (unsigned char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0F]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
}

}  // namespace

JsonObject parseFlatJsonObject(const std::string& text) {
    JsonObject obj;
    Parser parser(text);
    if (!parser.parse(obj)) {
        return JsonObject{};
    }
    return obj;
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    appendEscaped(out, value);
    return out;
}

std::string buildStatusJson(bool online, bool playing, const std::string& layout,
                            const std::string& layoutSetting, const std::vector<StreamInfo>& streams) {
    std::string out;
    out.reserve(64 + streams.size() * 48);
    out += "{\"online\":";
    out += online ? "true" : "false";
    out += ",\"playing\":";
    out += playing ? "true" : "false";
    out += ",\"layout\":";
    appendEscaped(out, layout);
    out += ",\"layout_setting\":";
    appendEscaped(out, layoutSetting);
    out += ",\"streams\":[";
    for (size_t i = 0; i < streams.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        const StreamInfo& stream = streams[i];
        out += "{\"id\":";
        appendEscaped(out, stream.id);
        out += ",\"url\":";
        appendEscaped(out, stream.url);
        out += ",\"muted\":";
        out += stream.muted ? "true" : "false";
        out.push_back('}');
    }
    out += "]}";
    return out;
}

std::string buildResponseJson(bool ok, const std::string& command, const std::string& id,
                              const std::string& message) {
    std::string out;
    out += "{\"ok\":";
    out += ok ? "true" : "false";
    out += ",\"command\":";
    appendEscaped(out, command);
    out += ",\"id\":";
    appendEscaped(out, id);
    out += ",\"message\":";
    appendEscaped(out, message);
    out.push_back('}');
    return out;
}
