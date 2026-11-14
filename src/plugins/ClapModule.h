#pragma once
#include "PluginInternal.h"
// Pinned MIT declarations: third_party/licenses/clap.txt
#include <clap/clap.h>
namespace cyaudio::detail {
struct ClapModule {
    SharedModule library;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
    bool initialized = false;
    ~ClapModule();
};
Status loadClapModule(const std::filesystem::path&, std::shared_ptr<ClapModule>&,
                      bool requirePluginFactory = true);
} // namespace cyaudio::detail
