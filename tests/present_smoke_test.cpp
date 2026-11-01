#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Black-box check of the --url presenter. Generates a tiny local file with
// the ffmpeg CLI, presents two frames through the software renderer, and
// checks that --renderer drm fails without opening a successful scanout.
// Exit 77 when the ffmpeg CLI is missing so ctest can skip.

#ifndef PLAYER_BIN
#define PLAYER_BIN ""
#endif

#ifndef FFMPEG_CLI
#define FFMPEG_CLI ""
#endif

namespace {

constexpr int kSkip = 77;

class TempDir {
public:
    TempDir()
        : path_(std::filesystem::temp_directory_path() /
                ("amlogic-present-smoke-" + std::to_string(static_cast<long long>(::getpid())))) {
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

int run(const std::vector<std::string>& args,
        const std::filesystem::path& outFile,
        const std::filesystem::path& errFile) {
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
        const int outFd = ::open(outFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int errFd = ::open(errFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outFd >= 0) {
            dup2(outFd, STDOUT_FILENO);
            if (outFd != STDOUT_FILENO) {
                ::close(outFd);
            }
        }
        if (errFd >= 0) {
            dup2(errFd, STDERR_FILENO);
            if (errFd != STDERR_FILENO) {
                ::close(errFd);
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

bool ppmLooksValid(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string header(64, '\0');
    in.read(header.data(), static_cast<std::streamsize>(header.size()));
    const auto n = in.gcount();
    if (n <= 0) {
        return false;
    }
    header.resize(static_cast<std::size_t>(n));
    return header.compare(0, 3, "P6\n") == 0 && header.find("\n255\n") != std::string::npos &&
           std::filesystem::file_size(path) > 32;
}

}  // namespace

int main() {
    try {
        const char* player = PLAYER_BIN;
        if (player == nullptr || player[0] == '\0') {
            std::cerr << "PLAYER_BIN is not set\n";
            return 1;
        }

        TempDir tmp;
        const std::filesystem::path log = tmp.path() / "ffmpeg.log";
        const std::filesystem::path empty = tmp.path() / "empty.log";
        const std::filesystem::path mp4 = tmp.path() / "clip.mp4";
        const int gen = run({ffmpegBinary(), "-y", "-loglevel", "error", "-f", "lavfi", "-i",
                             "testsrc=size=160x120:rate=10:duration=0.4", "-c:v", "mpeg4", "-pix_fmt",
                             "yuv420p", mp4.string()},
                            empty, log);
        if (gen == 127) {
            std::cout << "SKIP ffmpeg CLI not found\n";
            return kSkip;
        }
        if (gen != 0) {
            std::cerr << "failed to generate fixture (exit " << gen << ")\n" << readFile(log);
            return 1;
        }

        const std::filesystem::path frames = tmp.path() / "frames";
        const std::filesystem::path out = tmp.path() / "present.out";
        const std::filesystem::path err = tmp.path() / "present.err";
        const int presented = run({player, "--url", mp4.string(), "--frames", "2", "--render-dir", frames.string()},
                                  out, err);
        const std::string presentOut = readFile(out);
        const std::string presentErr = readFile(err);
        if (presented != 0) {
            std::cerr << "software present failed (exit " << presented << ")\n" << presentOut << presentErr;
            return 1;
        }
        if (presentOut.find("Presenter software (no /dev/dri)") == std::string::npos ||
            presentOut.find("fmt=yuv420p") == std::string::npos ||
            presentOut.find("layout 1x1") == std::string::npos ||
            presentOut.find("rgbsum=") == std::string::npos ||
            presentOut.find("Decoded 2 frame(s)") == std::string::npos) {
            std::cerr << "software present output missing expected lines\n" << presentOut;
            return 1;
        }
        if (!ppmLooksValid(frames / "frame-000000.ppm") || !ppmLooksValid(frames / "frame-000001.ppm")) {
            std::cerr << "expected two PPM frames in " << frames << "\n";
            return 1;
        }

        const std::filesystem::path drmOut = tmp.path() / "drm.out";
        const std::filesystem::path drmErr = tmp.path() / "drm.err";
        const int drm = run({player, "--renderer", "drm", "--url", mp4.string(), "--frames", "1"}, drmOut, drmErr);
        const std::string drmErrText = readFile(drmErr);
        if (drm == 0 || drmErrText.find("requires GS-King X / CoreELEC") == std::string::npos) {
            std::cerr << "drm presenter should fail closed without a GS-King X (exit " << drm << ")\n"
                      << readFile(drmOut) << drmErrText;
            return 1;
        }

        const int help = run({player, "--help", "--url", "example"}, out, err);
        const std::string helpText = readFile(out) + readFile(err);
        if (help != 0 || helpText.find("--render-dir") == std::string::npos) {
            std::cerr << "decode help missing --render-dir (exit " << help << ")\n" << helpText;
            return 1;
        }

        const int noUrl = run({player, "--render-dir", frames.string()}, out, err);
        if (noUrl != 2) {
            std::cerr << "--render-dir without --url should exit 2, got " << noUrl << "\n"
                      << readFile(out) << readFile(err);
            return 1;
        }

        const int mqttHelp = run({player, "--help"}, out, err);
        const std::string mqttText = readFile(out) + readFile(err);
        if (mqttHelp != 0 || mqttText.find("MQTT") == std::string::npos) {
            std::cerr << "default --help should stay on the MQTT path (exit " << mqttHelp << ")\n" << mqttText;
            return 1;
        }

        std::cout << "present_smoke_test ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL exception " << ex.what() << "\n";
        return 1;
    }
}
