#include <cyaudio/plugins/PluginWorker.h>
#include <string_view>
int main(int argc, char** argv) {
    return argc == 3 && std::string_view(argv[1]) == "--cydsp-worker"
               ? cyaudio::runPluginWorker(argv[2])
               : 2;
}
