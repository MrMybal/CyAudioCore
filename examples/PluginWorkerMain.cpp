#include <cyaudio/plugins/PluginWorker.h>
#include <string_view>

// A dedicated helper avoids initializing the application's engine or UI in a child.
int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--cydsp-worker")
        return 2;
    return cyaudio::runPluginWorker(argv[2]);
}
