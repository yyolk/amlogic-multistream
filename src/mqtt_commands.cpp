#include "mqtt_commands.h"

#include <string>

namespace {

std::string asciiLower(std::string value) {
    for (char& c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 'A' && uc <= 'Z') {
            c = static_cast<char>(uc - 'A' + 'a');
        }
    }
    return value;
}

CommandOutcome fail(std::string command, std::string id, std::string message) {
    CommandOutcome out;
    out.command = std::move(command);
    out.id = std::move(id);
    out.message = std::move(message);
    return out;
}

// MQTT add stores the URL for a later decoder. Accept only http and https,
// and reject a NUL so a prefix check cannot disagree with a C string.
bool mqttUrlAllowed(const std::string& url) {
    if (url.find('\0') != std::string::npos) {
        return false;
    }
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

}  // namespace

TopicCommand commandFromTopic(const std::string& prefix, const std::string& topic) {
    const std::string root = prefix + "/cmd";
    if (topic == root || topic == root + "/command") {
        TopicCommand matched;
        matched.matched = true;
        matched.fromPayload = true;
        return matched;
    }

    const std::string base = root + "/";
    if (topic.compare(0, base.size(), base) != 0) {
        return {};
    }

    TopicCommand matched;
    matched.matched = true;
    matched.name = topic.substr(base.size());
    if (matched.name.empty() || matched.name == "command") {
        matched.fromPayload = true;
        matched.name.clear();
    }
    return matched;
}

std::string resolveCommandName(const TopicCommand& topic, const JsonObject& payload) {
    if (!topic.matched) {
        return {};
    }
    if (!topic.fromPayload) {
        return asciiLower(topic.name);
    }
    std::string action = payload.getString("action");
    if (action.empty()) {
        action = payload.getString("command");
    }
    return asciiLower(action);
}

CommandOutcome executeCommand(StreamManager& streams, bool& playing, const std::string& command,
                              const JsonObject& payload) {
    if (command.empty()) {
        return fail(command, "", "command is required");
    }

    if (command == "start" || command == "play") {
        playing = true;
        CommandOutcome out;
        out.ok = true;
        out.command = command;
        out.message = "playing";
        out.publishStatus = true;
        return out;
    }

    if (command == "stop") {
        playing = false;
        CommandOutcome out;
        out.ok = true;
        out.command = command;
        out.message = "stopped";
        out.publishStatus = true;
        return out;
    }

    if (command == "add") {
        const std::string id = payload.getString("id");
        const std::string url = payload.getString("url");
        if (id.empty() || url.empty()) {
            return fail(command, id, "id and url are required");
        }
        if (!mqttUrlAllowed(url)) {
            return fail(command, id, "url must be http:// or https://");
        }
        const size_t before = streams.list().size();
        if (!streams.addStream(id, url)) {
            return fail(command, id, "stream already exists");
        }
        CommandOutcome out;
        out.ok = true;
        out.command = command;
        out.id = id;
        out.message = streams.list().size() == before ? "already present" : "added";
        out.publishStatus = true;
        return out;
    }

    if (command == "remove") {
        const std::string id = payload.getString("id");
        if (id.empty()) {
            return fail(command, "", "id is required");
        }
        if (!streams.removeStream(id)) {
            return fail(command, id, "stream not found");
        }
        CommandOutcome out;
        out.ok = true;
        out.command = command;
        out.id = id;
        out.message = "removed";
        out.publishStatus = true;
        return out;
    }

    if (command == "list") {
        CommandOutcome out;
        out.ok = true;
        out.command = command;
        out.message = "ok";
        out.publishStatus = true;
        return out;
    }

    if (command == "layout") {
        const auto layoutField = payload.strings.find("layout");
        if (layoutField == payload.strings.end()) {
            if (payload.has("layout")) {
                return fail(command, "", "layout must be a non-empty string");
            }
            CommandOutcome out;
            out.ok = true;
            out.command = command;
            out.message = streams.layout();
            out.publishStatus = false;
            return out;
        }
        const std::string& layout = layoutField->second;
        if (layout.empty() || !streams.setLayout(layout)) {
            return fail(command, "", "layout must be a non-empty string");
        }
        CommandOutcome out;
        out.ok = true;
        out.command = command;
        out.message = streams.layout();
        out.publishStatus = true;
        return out;
    }

    if (command == "mute") {
        const std::string id = payload.getString("id");
        if (id.empty()) {
            return fail(command, "", "id is required");
        }
        bool muted = false;
        if (!payload.getBool("muted", muted)) {
            return fail(command, id, "muted is required");
        }
        if (!streams.muteStream(id, muted)) {
            return fail(command, id, "stream not found");
        }
        CommandOutcome out;
        out.ok = true;
        out.command = command;
        out.id = id;
        out.message = muted ? "muted" : "unmuted";
        out.publishStatus = true;
        return out;
    }

    if (command == "restart") {
        // A present id that is empty, non-string, or unknown must not fall
        // through to restartAll(). Omitting the field restarts every stream.
        if (payload.has("id")) {
            const std::string id = payload.getString("id");
            if (id.empty()) {
                return fail(command, "", "id is required");
            }
            if (!streams.restartStream(id)) {
                return fail(command, id, "stream not found");
            }
            CommandOutcome out;
            out.ok = true;
            out.command = command;
            out.id = id;
            out.message = "restart requested";
            out.publishStatus = true;
            return out;
        }
        const int count = streams.restartAll();
        CommandOutcome out;
        out.ok = true;
        out.command = command;
        out.message = "restarted " + std::to_string(count);
        out.publishStatus = true;
        return out;
    }

    return fail(command, payload.getString("id"), "unknown command");
}
