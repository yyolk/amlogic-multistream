#include "mqtt_commands.h"
#include "mqtt_json.h"
#include "stream_manager.h"

#include <iostream>
#include <string>

namespace {

int g_failed = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "FAIL " << file << ":" << line << " " << expr << "\n";
        ++g_failed;
    }
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

JsonObject mustParse(const std::string& text) {
    JsonObject obj = parseFlatJsonObject(text);
    CHECK(obj.valid);
    return obj;
}

}  // namespace

int main() {
    CHECK(parseFlatJsonObject("").valid);
    CHECK(parseFlatJsonObject(" \n\t ").valid);
    CHECK(parseFlatJsonObject("{}").valid);
    CHECK(!parseFlatJsonObject("{").valid);
    CHECK(!parseFlatJsonObject("{\"id\":}").valid);
    CHECK(!parseFlatJsonObject("{\"id\":\"a\",}").valid);
    CHECK(!parseFlatJsonObject("[]").valid);
    CHECK(!parseFlatJsonObject("\"hello\"").valid);
    CHECK(!parseFlatJsonObject("{\"id\":\"a\"} junk").valid);
    CHECK(!parseFlatJsonObject("{\"muted\":trueX}").valid);

    const JsonObject cam = mustParse(R"({"id":"cam1","url":"http://x/a.m3u8","muted":true})");
    CHECK(cam.getString("id") == "cam1");
    CHECK(cam.getString("url") == "http://x/a.m3u8");
    bool muted = false;
    CHECK(cam.getBool("muted", muted) && muted);

    const JsonObject spaced = mustParse(" { \"id\" : \"cam\" , \"muted\" : false } ");
    CHECK(spaced.getString("id") == "cam");
    bool unmuted = true;
    CHECK(spaced.getBool("muted", unmuted) && !unmuted);

    const JsonObject lastWins = mustParse(R"({"id":"first","id":"second","url":null,"muted":1})");
    CHECK(lastWins.getString("id") == "second");
    CHECK(!lastWins.has("url"));
    bool ignoredNumber = false;
    CHECK(!lastWins.getBool("muted", ignoredNumber));

    const JsonObject escaped = mustParse(R"({"id":"a\"b","url":"line\n","ch":"\u0041","e":"\u00e9"})");
    CHECK(escaped.getString("id") == "a\"b");
    CHECK(escaped.getString("url") == "line\n");
    CHECK(escaped.getString("ch") == "A");
    CHECK(escaped.getString("e") == "\xC3\xA9");

    const JsonObject nested = mustParse(R"({"id":"cam","meta":{"k":1},"tags":["a","b"],"url":"http://x"})");
    CHECK(nested.getString("id") == "cam");
    CHECK(nested.getString("url") == "http://x");
    CHECK(!nested.has("meta"));
    CHECK(!nested.has("tags"));

    std::string deep = "{\"a\":";
    for (int i = 0; i < 40; ++i) {
        deep += "{\"a\":";
    }
    deep += "1";
    for (int i = 0; i < 41; ++i) {
        deep += "}";
    }
    CHECK(!parseFlatJsonObject(deep).valid);
    CHECK(parseFlatJsonObject(R"({"layout":"2x2","extra":{"ok":true}})").valid);

    StreamInfo quoted{"a\"b", "http://x/y", true};
    CHECK(buildStatusJson(true, false, "1x1", "auto", {quoted}) ==
          R"({"online":true,"playing":false,"layout":"1x1","layout_setting":"auto","streams":[{"id":"a\"b","url":"http://x/y","muted":true}]})");
    CHECK(buildResponseJson(false, "add", "cam1", "stream already exists") ==
          R"({"ok":false,"command":"add","id":"cam1","message":"stream already exists"})");
    CHECK(jsonEscape("a\"b\\") == "\"a\\\"b\\\\\"");

    const std::string prefix = "amlogic-multistream";
    const TopicCommand addTopic = commandFromTopic(prefix, prefix + "/cmd/add");
    CHECK(addTopic.matched && !addTopic.fromPayload && addTopic.name == "add");
    const TopicCommand bare = commandFromTopic(prefix, prefix + "/cmd");
    CHECK(bare.matched && bare.fromPayload && bare.name.empty());
    const TopicCommand viaCommand = commandFromTopic(prefix, prefix + "/cmd/command");
    CHECK(viaCommand.matched && viaCommand.fromPayload);
    CHECK(!commandFromTopic(prefix, prefix + "/status").matched);
    CHECK(!commandFromTopic(prefix, "amlogic-multistream-other/cmd/add").matched);
    CHECK(!commandFromTopic("amlogic", "amlogic-multistream/cmd/add").matched);

    TopicCommand upper;
    upper.matched = true;
    upper.name = "ADD";
    CHECK(resolveCommandName(upper, JsonObject{}) == "add");

    TopicCommand fromPayload;
    fromPayload.matched = true;
    fromPayload.fromPayload = true;
    CHECK(resolveCommandName(fromPayload, mustParse(R"({"action":"Play"})")) == "play");
    CHECK(resolveCommandName(fromPayload, mustParse(R"({"command":"STOP"})")) == "stop");

    TopicCommand topicWins;
    topicWins.matched = true;
    topicWins.name = "add";
    CHECK(resolveCommandName(topicWins, mustParse(R"({"action":"stop"})")) == "add");

    StreamManager streams;
    bool playing = false;

    auto missing = executeCommand(streams, playing, "", JsonObject{});
    CHECK(!missing.ok && missing.message == "command is required");

    auto unknown = executeCommand(streams, playing, "nope", mustParse(R"({"id":"x"})"));
    CHECK(!unknown.ok && unknown.message == "unknown command" && !unknown.publishStatus);

    auto badAdd = executeCommand(streams, playing, "add", mustParse(R"({"id":"cam1"})"));
    CHECK(!badAdd.ok && badAdd.message == "id and url are required");

    auto added = executeCommand(streams, playing, "add",
                                mustParse(R"({"id":"cam1","url":"http://example/a.m3u8"})"));
    CHECK(added.ok && added.message == "added" && added.publishStatus);
    auto again = executeCommand(streams, playing, "add",
                                mustParse(R"({"id":"cam1","url":"http://example/a.m3u8"})"));
    CHECK(again.ok && again.message == "already present");
    auto conflict = executeCommand(streams, playing, "add",
                                   mustParse(R"({"id":"cam1","url":"http://example/b.m3u8"})"));
    CHECK(!conflict.ok && conflict.message == "stream already exists");

    auto added2 = executeCommand(streams, playing, "add",
                                 mustParse(R"({"id":"cam2","url":"http://example/c.m3u8"})"));
    CHECK(added2.ok);
    const auto listed = streams.list();
    CHECK(listed.size() == 2);
    CHECK(listed[0].id == "cam1" && listed[1].id == "cam2");
    CHECK(!listed[0].muted && listed[0].url == "http://example/a.m3u8");

    auto layoutQuery = executeCommand(streams, playing, "layout", mustParse("{}"));
    CHECK(layoutQuery.ok && !layoutQuery.publishStatus && layoutQuery.message == "1x2");
    CHECK(streams.layoutSetting() == "auto");

    auto layoutSet = executeCommand(streams, playing, "layout", mustParse(R"({"layout":"2x2"})"));
    CHECK(layoutSet.ok && layoutSet.publishStatus && layoutSet.message == "2x2");
    CHECK(streams.layout() == "2x2" && streams.layoutSetting() == "2x2");

    auto layoutBad = executeCommand(streams, playing, "layout", mustParse(R"({"layout":""})"));
    CHECK(!layoutBad.ok && layoutBad.message == "layout must be a non-empty string");
    CHECK(streams.layoutSetting() == "2x2");

    auto layoutAuto = executeCommand(streams, playing, "layout", mustParse(R"({"layout":"auto"})"));
    CHECK(layoutAuto.ok && layoutAuto.message == "1x2");
    CHECK(streams.layoutSetting() == "auto");

    auto muteBad = executeCommand(streams, playing, "mute", mustParse(R"({"id":"cam1"})"));
    CHECK(!muteBad.ok && muteBad.message == "muted is required");
    auto muteMissing = executeCommand(streams, playing, "mute", mustParse(R"({"id":"missing","muted":true})"));
    CHECK(!muteMissing.ok && muteMissing.message == "stream not found");
    auto mute = executeCommand(streams, playing, "mute", mustParse(R"({"id":"cam1","muted":true})"));
    CHECK(mute.ok && mute.message == "muted" && streams.list()[0].muted);
    auto unmute = executeCommand(streams, playing, "mute", mustParse(R"({"id":"cam1","muted":"false"})"));
    CHECK(unmute.ok && unmute.message == "unmuted" && !streams.list()[0].muted);

    auto restartMissing = executeCommand(streams, playing, "restart", mustParse(R"({"id":"missing"})"));
    CHECK(!restartMissing.ok && restartMissing.message == "stream not found");
    CHECK(streams.list()[0].restartGeneration == 0);
    CHECK(streams.list()[1].restartGeneration == 0);
    auto restartEmpty = executeCommand(streams, playing, "restart", mustParse(R"({"id":""})"));
    CHECK(!restartEmpty.ok && restartEmpty.message == "id is required");
    auto restartBool = executeCommand(streams, playing, "restart", mustParse(R"({"id":true})"));
    CHECK(!restartBool.ok && restartBool.message == "id is required");
    CHECK(streams.list()[0].restartGeneration == 0);
    CHECK(streams.list()[1].restartGeneration == 0);
    streams.muteStream("cam1", true);
    auto restartOne = executeCommand(streams, playing, "restart", mustParse(R"({"id":"cam1"})"));
    CHECK(restartOne.ok && restartOne.message == "restart requested");
    CHECK(streams.list()[0].muted);
    CHECK(streams.list()[0].url == "http://example/a.m3u8");
    CHECK(streams.list()[0].restartGeneration == 1);
    CHECK(streams.list()[1].restartGeneration == 0);
    auto restartAll = executeCommand(streams, playing, "restart", mustParse("{}"));
    CHECK(restartAll.ok && restartAll.message == "restarted 2");
    CHECK(streams.list().size() == 2 && streams.list()[0].muted);
    CHECK(streams.list()[0].restartGeneration == 2);
    CHECK(streams.list()[1].restartGeneration == 1);

    auto listCmd = executeCommand(streams, playing, "list", mustParse(""));
    CHECK(listCmd.ok && listCmd.message == "ok" && listCmd.publishStatus);

    CHECK(!playing);
    CHECK(streams.list().size() == 2);
    auto play = executeCommand(streams, playing, "play", mustParse("{}"));
    CHECK(play.ok && play.message == "playing" && playing && play.publishStatus);
    CHECK(streams.list().size() == 2);
    auto stop = executeCommand(streams, playing, "stop", mustParse(""));
    CHECK(stop.ok && stop.message == "stopped" && !playing);
    auto start = executeCommand(streams, playing, "start", JsonObject{});
    CHECK(start.ok && playing);

    auto removed = executeCommand(streams, playing, "remove", mustParse(R"({"id":"cam1"})"));
    CHECK(removed.ok && removed.message == "removed");
    CHECK(streams.list().size() == 1 && streams.list()[0].id == "cam2");
    auto removeMissing = executeCommand(streams, playing, "remove", mustParse(R"({"id":"cam1"})"));
    CHECK(!removeMissing.ok && removeMissing.message == "stream not found");
    CHECK(!executeCommand(streams, playing, "remove", mustParse("{}")).ok);

    CHECK(buildStatusJson(true, playing, streams.layout(), streams.layoutSetting(), streams.list())
              .find("\"id\":\"cam2\"") != std::string::npos);

    // add must not store a URL the decoder could open as a local or raw protocol.
    const size_t beforeReject = streams.list().size();
    const char* rejected[] = {
        "file:///etc/hostname",
        "pipe:0",
        "tcp://127.0.0.1:9",
        "concat:file:///etc/hostname",
        "HTTP://example/a.m3u8",
    };
    int rejectIndex = 0;
    for (const char* url : rejected) {
        const std::string payload =
            std::string("{\"id\":\"bad") + std::to_string(rejectIndex) + "\",\"url\":\"" + url + "\"}";
        auto denied = executeCommand(streams, playing, "add", mustParse(payload));
        CHECK(!denied.ok);
        CHECK(denied.message == "url must be http:// or https://");
        ++rejectIndex;
    }
    auto nulUrl = executeCommand(
        streams, playing, "add",
        mustParse("{\"id\":\"badnul\",\"url\":\"http://example/a\\u0000.m3u8\"}"));
    CHECK(!nulUrl.ok);
    CHECK(nulUrl.message == "url must be http:// or https://");
    CHECK(streams.list().size() == beforeReject);

    auto https = executeCommand(
        streams, playing, "add",
        mustParse(R"({"id":"secure","url":"https://example/live.m3u8"})"));
    CHECK(https.ok && https.message == "added");
    CHECK(streams.list().size() == beforeReject + 1);
    CHECK(streams.list().back().url == "https://example/live.m3u8");

    if (g_failed != 0) {
        std::cerr << g_failed << " check(s) failed\n";
        return 1;
    }
    std::cout << "mqtt smoke test passed\n";
    return 0;
}
