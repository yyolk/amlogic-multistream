#pragma once
#include "mqtt_json.h"

#include <string>

struct TopicCommand {
    bool matched = false;
    // True when the command name comes from the payload "action" or "command" field.
    bool fromPayload = false;
    std::string name;
};

struct CommandOutcome {
    bool ok = false;
    std::string command;
    std::string id;
    std::string message;
    bool publishStatus = false;
};

// Match "<prefix>/cmd/#". The bare "<prefix>/cmd" topic and ".../cmd/command"
// take the command name from the payload.
TopicCommand commandFromTopic(const std::string& prefix, const std::string& topic);

// Topic name wins over a payload action. Names are lowercased.
std::string resolveCommandName(const TopicCommand& topic, const JsonObject& payload);

// Apply one command to in-memory stream state. `playing` is the player-loop flag.
// While it is true, StreamPlayer opens a software decoder per visible stream.
// `add` stores only http:// and https:// URLs, and rejects a NUL in the URL,
// so a non-network URL never enters the table a decoder would open.
// A new id past LayoutEngine::kMaxStreams (9) fails with "stream limit reached"
// and does not publish status. Repeating an id already in the table is unchanged.
// `layout` stores "auto" or a token LayoutEngine::parseLayout accepts.
// "banana", "4", and "2X2" fail, leave the previous grid in place, and do not
// publish status, so the wall is not cleared.
// `restart` with an id bumps that stream's generation so the player reopens
// its decoder. An unknown, empty, or non-string id fails and does not restart
// any other stream. Omitting id restarts every stream.
CommandOutcome executeCommand(StreamManager& streams, bool& playing, const std::string& command,
                              const JsonObject& payload);
