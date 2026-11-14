#pragma once
#include <cstdint>
// Original test-only control surface, never installed or linked into production.
struct ClapFixtureStats {
    std::uint32_t entryInit = 0, entryDeinit = 0, created = 0, destroyed = 0, providerCreated = 0,
                  providerDestroyed = 0, stateSaveContext = 0, stateLoadContext = 0,
                  trackQueries = 0, mapping = 0, automation = 0, poolTasks = 0, flushes = 0,
                  noteEnds = 0, renderMode = 0, guiTitles = 0, badGuiHide = 0, guiAcknowledged = 0;
};
enum class ClapFixtureCommand : std::uint32_t {
    normal,
    processSleep,
    processTail,
    processQuiet,
    processError,
    invalidOutput,
    invalidMetadata,
    failProviderInit,
    floodLogs,
    invalidateCookie,
    failActivation,
    hardRealtime,
    oversizeConfig,
    invalidSurround,
    concurrentLogs,
    guiDestroyed
};
using ClapFixtureRead = ClapFixtureStats (*)();
using ClapFixtureControl = void (*)(ClapFixtureCommand);
