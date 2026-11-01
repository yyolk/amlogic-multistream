#include "app.h"

#include <cstdio>
#include <iostream>

int main(int argc, char* argv[]) {
    // Line-buffered and flushed per insertion so a SIGKILL still leaves the
    // last log line. stdout is fully buffered when it is not a terminal.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::cout << std::unitbuf;
    std::cout << "amlogic-multistream starting...\n";
    App app;
    return app.run(argc, argv);
}
