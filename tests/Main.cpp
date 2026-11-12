#include <cyaudio/CyAudioCore.h>
#include <iostream>
#include <stdexcept>
void ciPropertyRegression(); void ciAdvancedRegression(); void dawMidiRegression();
void midi2ProtocolRegression(); void dawRegression(); void midiProtocolRegression(); void instrumentRegression();
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
int main(int, char** argv) {
 try {
  ciPropertyRegression(); ciAdvancedRegression(); dawMidiRegression(); midi2ProtocolRegression();
  dawRegression(); midiProtocolRegression(); instrumentRegression();
#if defined(CYAUDIOCORE_TEST_AUDIO_PROVIDER)
  audioProviderRegression();
#if CYAUDIOCORE_WITH_PORTAUDIO
  portAudioRegression();
#endif
#endif
#if defined(CYAUDIOCORE_TEST_IO)
  midi2NativeRegression(); deviceContractsRegression(); midiInputContractsRegression();
  pcmProxyRegression(); metadataRegression(); dawFilesRegression(); ioRegression();
#endif
#if defined(CYAUDIOCORE_TEST_SOURCES)
  sourcesRegression(); sourcePolicyRegression();
#endif
#if defined(CYAUDIOCORE_TEST_PLUGINS)
  const auto directory=std::filesystem::absolute(argv[0]).parent_path();
  pluginRegression(directory); openPluginRegression(directory); vstPluginRegression(directory); vstStateRegression(directory); pluginWorkerRegression(directory);
#else
  (void)argv;
#endif
  std::cout << "PASS independent CyAudioCore services/plugins/IPC/states/realtime\n";
  return 0;
 } catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
