#pragma once

#include <string>

namespace updater
{
    typedef void (*ProgressCallback)(const char* message, void* userData);

    enum class Result
    {
        NoUpdate,
        Updated,
        Failed,
    };

    // All application-specific values are supplied by the Switch launcher.
    struct Config
    {
        std::string applicationName;
        std::string configFileBaseName;
        std::string currentVersion;
        int currentBuild = 0;
        std::string defaultManifestUrl;
        // Path of the currently running executable. It is also used to
        // resolve the optional local update configuration file.
        std::string executablePath;
        std::string targetPath;
        std::string platform;
        // A launcher cannot replace its own executable while it is running.
        // When enabled, an update for executablePath is staged and installed
        // on the next launcher invocation, while all other files are
        // installed immediately.
        bool deferExecutableUpdate = false;
        bool networkAlreadyInitialized = false;
        bool logProgress = true;
        ProgressCallback progressCallback = nullptr;
        void* progressUserData = nullptr;
    };

    Result run(const Config& config);
}
