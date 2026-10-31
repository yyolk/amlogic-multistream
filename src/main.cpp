#include "app.h"
#include <iostream>

int main(int argc, char* argv[]) {
    std::cout << "amlogic-multistream starting...\n";
    App app;
    return app.run(argc, argv);
}
