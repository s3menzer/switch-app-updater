# Switch updater

`src/updater` is a Switch-only C++ update library. The application Switch
launcher uses it to update `game.nro`, `settings07.cfg`, and, when available,
the launcher `launcher.nro` before starting the game.

The updater is self-contained: its CMake target uses only the sources in this
directory, including the vendored JSON parser.
It does not include or compile any application engine source.

The Switch launcher is also implemented in this project as `launcher.cpp`.
The embedding build supplies the application version and manifest URL as
compile definitions. Before updating or chain-loading `game.nro`, the launcher
changes its working directory to the directory containing the selected
`launcher.nro`. It also passes the absolute `game.nro` path to the next
application so the game can restore that directory after hbloader starts it.

## Embedding

```cpp
#include "updater.hpp"

updater::Config config;
config.applicationName = "application";
config.configFileBaseName = "application";
config.currentVersion = "0.7.4";
config.currentBuild = 42;
config.defaultManifestUrl =
    "http://example.invalid/switch/application/manifests.json";
config.executablePath = "launcher.nro";
config.targetPath = "game.nro";
config.platform = "switch";
config.deferExecutableUpdate = true;
config.networkAlreadyInitialized = true;

const updater::Result result = updater::run(config);
```

`deferExecutableUpdate` stages an update for `executablePath`, because the
launcher cannot safely replace itself while it is running. Other manifest
files are installed immediately. A staged launcher is installed at the next
launcher invocation.

## Manifest format

The manifest is JSON. `files` may contain both the launcher and the game:

```json
{
  "name": "application",
  "platform": "switch",
  "version": "0.7.4",
  "build": 42,
  "files": [
    {
      "path": "game.nro",
      "download": "http://example.invalid/switch/application/game.nro",
      "sha256": "..."
    },
    {
      "path": "launcher.nro",
      "download": "http://example.invalid/switch/application/launcher.nro",
      "sha256": "..."
    },
    {
      "path": "settings07.cfg",
      "download": "http://example.invalid/switch/application/settings07.cfg",
      "sha256": "..."
    }
  ]
}
```

Paths must be relative and cannot contain empty components, `.`/`..`, a
backslash, a drive prefix, or start with `/`.

Manifests and optional local updater configuration files use JSON exclusively.

## Application Switch layout

```text
/switch/application/
  launcher.nro       # launcher started by hbmenu
  game.nro            # actual game
  settings07.cfg      # updateable default settings
  config.cfg          # mutable user settings
  manifests.json      # hosted update manifest, not required on the SD card
```

Static game assets are embedded in the NRO RomFS during the application build;
the `data/` directory therefore does not need to be copied to the SD card.
The launcher updates files in the directory containing the NROs and then
chain-loads `game.nro`. Updating the game or settings therefore does not
require restarting the launcher. If the launcher itself changed, the new
launcher is staged and installed on the next launch.

The optional local configuration file is
`<application>-update.json` next to the launcher. It may contain `manifest_url`
or `url`. Otherwise the configured default URL is used.
