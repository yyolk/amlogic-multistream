#include "ffmpeg_decoder.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Local software-decode smoke test. Generates a tiny fixture with the
// ffmpeg CLI (no network) and pulls one YUV420P frame through FfmpegDecoder.
// Exit 77 when the CLI is missing so ctest can mark the test skipped.
// FFmpeg dev libraries are required to link this binary; CMake omits the
// target when they are absent.

#ifndef FFMPEG_CLI
#define FFMPEG_CLI ""
#endif

namespace {

int g_failures = 0;
const char* g_case = "";

void check(bool cond, const char* expr, int line) {
    if (cond) {
        return;
    }
    std::cerr << "FAIL " << g_case << " " << expr << " (" << __FILE__ << ":" << line << ")\n";
    ++g_failures;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

constexpr int kSkip = 77;
constexpr int kWidth = 160;
constexpr int kHeight = 120;

class TempDir {
public:
    TempDir()
        : path_(std::filesystem::temp_directory_path() /
                ("amlogic-ffmpeg-decoder-" + std::to_string(static_cast<long long>(::getpid())))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Returns the child exit status, 127 if the executable cannot be run, or -1
// on a fork/wait failure. stderr is captured in err_file.
int run(const std::vector<std::string>& args, const std::filesystem::path& err_file) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        const int fd = ::open(err_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDERR_FILENO);
            if (fd != STDERR_FILENO) {
                ::close(fd);
            }
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (!WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

std::string ffmpegBinary() {
    const char* configured = FFMPEG_CLI;
    if (configured != nullptr && configured[0] != '\0') {
        return configured;
    }
    return "ffmpeg";
}

bool encoderMissing(const std::string& log) {
    const bool unknown = log.find("Unknown encoder") != std::string::npos;
    const bool not_found = log.find("not found") != std::string::npos;
    return unknown || (not_found && log.find("libx264") != std::string::npos);
}

void expectYuv420(const DecodedFrame& frame) {
    CHECK(frame.format == FramePixelFormat::Yuv420p);
    CHECK(frame.width == kWidth);
    CHECK(frame.height == kHeight);
    CHECK(frame.data[0] != nullptr);
    CHECK(frame.data[1] != nullptr);
    CHECK(frame.data[2] != nullptr);
    CHECK(frame.linesize[0] >= frame.width);
    const int chroma_w = (frame.width + 1) / 2;
    CHECK(frame.linesize[1] >= chroma_w);
    CHECK(frame.linesize[2] >= chroma_w);
    // Touch the planes so a bad pointer fails here instead of looking valid.
    const volatile uint8_t y = frame.data[0][0];
    const volatile uint8_t u = frame.data[1][0];
    const volatile uint8_t v = frame.data[2][0];
    (void)y;
    (void)u;
    (void)v;
}

void testDecode(const std::string& url) {
    FfmpegDecoder decoder;
    CHECK(decoder.isHardware() == false);
    // Without the CLI allowance a local path must not reach the demuxer.
    CHECK(!decoder.open(url));
    decoder.setAllowLocalFile(true);

    const bool opened = decoder.open(url);
    CHECK(opened);
    CHECK(decoder.isHardware() == false);
    if (!opened) {
        decoder.close();
        return;
    }

    int got = 0;
    for (; got < 32; ++got) {
        DecodedFrame frame;
        if (!decoder.readFrame(frame)) {
            break;
        }
        expectYuv420(frame);
    }
    CHECK(got >= 1);
    CHECK(got < 32);

    decoder.close();
    decoder.close();

    CHECK(decoder.open(url));
    DecodedFrame again;
    CHECK(decoder.readFrame(again));
    expectYuv420(again);
    decoder.close();
}

void testMissingFile(const std::filesystem::path& dir) {
    g_case = "missing file";
    FfmpegDecoder decoder;
    decoder.setAllowLocalFile(true);
    const std::string url = (dir / "no-such-file.mp4").string();
    CHECK(!decoder.open(url));
    CHECK(decoder.isHardware() == false);
    decoder.close();
    decoder.close();
}

}  // namespace

int main() {
    try {
        const std::string ffmpeg = ffmpegBinary();
        TempDir tmp;
        const std::filesystem::path log = tmp.path() / "ffmpeg.log";
        const std::filesystem::path mp4 = tmp.path() / "clip.mp4";

        g_case = "generate mp4";
        const int mp4_rc = run(
            {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
             "-i", "testsrc=size=160x120:rate=10:duration=0.4",
             "-c:v", "mpeg4", "-pix_fmt", "yuv420p", mp4.string()},
            log);
        if (mp4_rc == 127) {
            std::cout << "SKIP ffmpeg CLI not found\n";
            return kSkip;
        }
        if (mp4_rc != 0) {
            std::cerr << "failed to generate MP4 fixture (exit " << mp4_rc << ")\n"
                      << readFile(log);
            return 1;
        }

        g_case = "mp4";
        testDecode(mp4.string());
        testMissingFile(tmp.path());

        g_case = "generate hls";
        const std::filesystem::path hls_dir = tmp.path() / "hls";
        std::filesystem::create_directories(hls_dir);
        const std::filesystem::path playlist = hls_dir / "index.m3u8";
        const int hls_rc = run(
            {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
             "-i", "testsrc=size=160x120:rate=10:duration=0.4",
             "-c:v", "libx264", "-pix_fmt", "yuv420p",
             "-hls_time", "1", "-hls_list_size", "0", playlist.string()},
            log);
        bool hls_decoded = false;
        if (hls_rc == 0) {
            g_case = "hls";
            testDecode(playlist.string());
            hls_decoded = true;
        } else if (hls_rc == 127) {
            std::cout << "SKIP ffmpeg CLI not found\n";
            return kSkip;
        } else {
            const std::string hls_log = readFile(log);
            if (!encoderMissing(hls_log)) {
                std::cerr << "failed to generate HLS fixture (exit " << hls_rc << ")\n" << hls_log;
                return 1;
            }
            std::cout << "HLS fixture skipped: libx264 encoder not available\n";
        }

        if (g_failures != 0) {
            std::cerr << g_failures << " check(s) failed\n";
            return 1;
        }
        std::cout << "ffmpeg_decoder_tests ok (" << (hls_decoded ? "mp4, hls" : "mp4") << ")\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL exception " << ex.what() << "\n";
        return 1;
    }
}
