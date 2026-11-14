#include <cyaudio/CyAudioCore.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
void ciPropertyRegression(); void ciAdvancedRegression(); void dawMidiRegression();
void midi2ProtocolRegression(); void dawRegression(); void midiProtocolRegression(); void instrumentRegression();
void synthWavetableRegression();
void dspCompositionRegression();
#if defined(CYAUDIOCORE_TEST_AUDIO_PROVIDER)
void audioProviderRegression();
#if CYAUDIOCORE_WITH_PORTAUDIO
void portAudioRegression();
#endif
#endif
#if defined(CYAUDIOCORE_TEST_IO)
void midi2NativeRegression(); void deviceContractsRegression(); void midiInputContractsRegression();
void pcmProxyRegression(); void metadataRegression(); void dawFilesRegression(); void ioRegression();
#endif
#if defined(CYAUDIOCORE_TEST_SOURCES)
void sourcesRegression(); void sourcePolicyRegression();
#endif
#if defined(CYAUDIOCORE_TEST_PLUGINS)
void pluginRegression(const std::filesystem::path&); void openPluginRegression(const std::filesystem::path&);
void vstStateRegression(const std::filesystem::path&);
void vstPluginRegression(const std::filesystem::path&); void pluginWorkerRegression(const std::filesystem::path&);
#endif
namespace {
// Suites run in this order. Names are the command-line selectors and the CTest entries.
struct Suite {
 const char* name;
 void (*run)(const std::filesystem::path&);
};
template <void (*F)()> void plain(const std::filesystem::path&) { F(); }
const Suite suites[] = {
 {"ciProperty", &plain<&ciPropertyRegression>}, {"ciAdvanced", &plain<&ciAdvancedRegression>},
 {"dawMidi", &plain<&dawMidiRegression>}, {"midi2Protocol", &plain<&midi2ProtocolRegression>},
 {"daw", &plain<&dawRegression>}, {"midiProtocol", &plain<&midiProtocolRegression>},
 {"instrument", &plain<&instrumentRegression>}, {"synthWavetable", &plain<&synthWavetableRegression>},
 {"dspComposition", &plain<&dspCompositionRegression>},
#if defined(CYAUDIOCORE_TEST_AUDIO_PROVIDER)
 {"audioProvider", &plain<&audioProviderRegression>},
#if CYAUDIOCORE_WITH_PORTAUDIO
 {"portAudio", &plain<&portAudioRegression>},
#endif
#endif
#if defined(CYAUDIOCORE_TEST_IO)
 {"midi2Native", &plain<&midi2NativeRegression>}, {"deviceContracts", &plain<&deviceContractsRegression>},
 {"midiInputContracts", &plain<&midiInputContractsRegression>}, {"pcmProxy", &plain<&pcmProxyRegression>},
 {"metadata", &plain<&metadataRegression>}, {"dawFiles", &plain<&dawFilesRegression>}, {"io", &plain<&ioRegression>},
#endif
#if defined(CYAUDIOCORE_TEST_SOURCES)
 {"sources", &plain<&sourcesRegression>}, {"sourcePolicy", &plain<&sourcePolicyRegression>},
#endif
#if defined(CYAUDIOCORE_TEST_PLUGINS)
 {"plugin", &pluginRegression}, {"openPlugin", &openPluginRegression}, {"vstPlugin", &vstPluginRegression},
 {"vstState", &vstStateRegression}, {"pluginWorker", &pluginWorkerRegression},
#endif
};
}
// No argument: every suite, stopping at the first failure (unchanged behaviour).
// Suite names select suites; --keep-going runs the rest after a failure; --list prints the names.
int main(int argc, char** argv) {
 bool keepGoing = false, selected = false;
 for (int i = 1; i < argc; ++i) {
  if (!std::strcmp(argv[i], "--list")) {
   for (const auto& suite : suites) std::cout << suite.name << '\n';
   return 0;
  }
  if (!std::strcmp(argv[i], "--keep-going")) { keepGoing = true; continue; }
  bool known = false;
  for (const auto& suite : suites) known = known || !std::strcmp(argv[i], suite.name);
  if (!known) { std::cerr << "FAIL: unknown suite " << argv[i] << '\n'; return 2; }
  selected = true;
 }
 const auto directory = std::filesystem::absolute(argv[0]).parent_path();
 unsigned failures = 0;
 for (const auto& suite : suites) {
  bool wanted = !selected;
  for (int i = 1; i < argc && !wanted; ++i) wanted = !std::strcmp(argv[i], suite.name);
  if (!wanted) continue;
  try { suite.run(directory); }
  catch (const std::exception& e) {
   std::cerr << "FAIL: " << suite.name << ": " << e.what() << '\n';
   ++failures;
   if (!keepGoing) return 1;
  }
 }
 if (failures) { std::cerr << "FAIL: " << failures << " suite(s)\n"; return 1; }
 if (!selected) std::cout << "PASS independent CyAudioCore services/plugins/IPC/states/realtime\n";
 return 0;
}
