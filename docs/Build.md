# Build SpeechDesk

SpeechDesk 1.1.1 uses C++17, Qt, and a pinned static libsodium build. Build the crypto dependency before configuring the application. The commands below run from the repository root.

## Requirements

- CMake 3.21 or later and Ninja.
- A C++17 compiler, plus a compatible C compiler and `make` for libsodium.
- Qt 6.4 or later with Core, Gui, Widgets, Network, **WebSockets**, Multimedia, and Svg. Linux also requires DBus; focused tests require Qt Test. Release packaging targets Qt 6.8.3.
- Python **3.12 or later** for the complete build and native-asset workflow. Hashing helpers use `hashlib.file_digest`, which requires at least Python 3.11; native source/bootstrap helpers require Python 3.12 for the supported archive-extraction path. Use 3.12 or later for all commands here.
- On Windows, the Qt MinGW compiler and MSYS2's shell and `make` utilities. The release toolchain uses MinGW 13.1.0. Use the same compiler family for libsodium and SpeechDesk.

The installed application does not require Python, MSYS2, or a separately installed Qt SDK. Those are build-time tools.

## Build on Linux

With Qt and the build tools installed:

```sh
python3 vendor/tools/build_sodium.py --output build/deps/sodium --cc gcc --jobs 3
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DSPEECHDESK_BUILD_TESTS=ON
cmake --build build --parallel 3
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure --timeout 90
```

If CMake cannot find your Qt SDK, add `-DCMAKE_PREFIX_PATH=/path/to/your/Qt` to the configure command. Its prefix must contain the Qt installation with the required modules, including WebSockets.

The application binary is `build/speechdesk`. This development build uses the Qt installation on the build machine; it is not yet a portable distribution.

## Build on Windows

Install the Qt 6.8.3 MinGW SDK with Multimedia, WebSockets, Svg, and the required shader tools. Ensure CMake, Ninja, and Python are available. In an MSYS2 terminal, install the build utility:

```sh
pacman -S --needed make
```

Then use **PowerShell**, adjusting these example SDK paths for your installation:

```powershell
$SpeechDeskQt = 'C:\Qt\6.8.3\mingw_64'
$SpeechDeskMingw = 'C:\Qt\Tools\mingw1310_64\bin'
$SpeechDeskMsys = 'C:\msys64\usr\bin'
$env:PATH = "$SpeechDeskMingw;$SpeechDeskQt\bin;$env:PATH"
python vendor/tools/build_sodium.py --output build/deps/sodium --cc "$SpeechDeskMingw\gcc.exe" --shell "$SpeechDeskMsys\bash.exe" --make "$SpeechDeskMsys\make.exe" --jobs 3
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=$SpeechDeskMingw\g++.exe" "-DCMAKE_PREFIX_PATH=$SpeechDeskQt" -DSPEECHDESK_BUILD_TESTS=ON
cmake --build build --parallel 3
$env:QT_QPA_PLATFORM = 'offscreen'
ctest --test-dir build --output-on-failure --timeout 90
```

The application binary is `build/SpeechDesk.exe`. Run the build and tests from the native Windows shell with Qt's MinGW runtime first on `PATH`. Git Bash can introduce different runtime DLLs and cause an entry-point error even when compilation succeeds. MSYS2 is used by the dependency helper for shell utilities; its compiler is not substituted for Qt's compiler.

## Pinned crypto dependency

`vendor/provenance/libsodium.json` records the required libsodium 1.0.22 source URL and SHA-256. `build_sodium.py` verifies the source archive, builds the static library, and writes a build receipt. CMake deliberately requires the pinned headers and `libsodium.a`; it does not silently choose a system library or DLL.

The default installation is `build/deps/sodium`. For another directory, pass the same location to the helper's `--output` and CMake's `-DSPEECHDESK_SODIUM_ROOT` option. Rebuild this dependency when changing compiler or target platform. Static linkage means the runtime package does not need a separate libsodium DLL.

## Add local recognition assets

Application compilation and fixture tests do not download recognition models. To use dictation in a source build, reconstruct the pinned assets on a Linux build host:

```sh
python3 vendor/tools/bootstrap_native.py --platform linux --jobs 3
cmake -S . -B build
cmake --build build --target stage_resources
```

Reconfiguring after the download registers the resource-staging target. For Windows assets, use `--platform windows` on the Linux build host and transfer the resulting `vendor/assets` tree to the Windows checkout before reconfiguring and staging. See [Native engines](Native-Engines.md) for pins, prerequisites, and supported targets.

## Distribution packages

Use the repository's **Release packages** workflow for Windows and Linux packages with Qt runtimes, recognition assets, source archives, and the single checksum manifest. The scripts under `packaging/` perform the corresponding staging and layout checks. Passing the fixture tests does not replace a physical microphone and speaker check on the intended desktop.
