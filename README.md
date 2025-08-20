# BallanceMMO

Build status:
[![Server](https://github.com/Swung0x48/BallanceMMO/actions/workflows/server.yml/badge.svg)](https://github.com/Swung0x48/BallanceMMO/actions/workflows/server.yml)
·
[![Client](https://github.com/Swung0x48/BallanceMMO/actions/workflows/client.yml/badge.svg)](https://github.com/Swung0x48/BallanceMMO/actions/workflows/client.yml)

## What is BallanceMMO?

BallanceMMO is a project which brings online experiences to Ballance.

## Getting Started

### Client

1. Install Ballance Mod Loader (aka. BML).
    - Download the latest version of BML [here](https://github.com/Gamepiaynmo/BallanceModLoader/releases).
    - Extract and put `BML.dll` under `$(game_directory)\BuildingBlocks`, where `$(game_directory)` is where you installed your game to.
2. Download the latest mod release from the [BMMO download site](https://dl.bmmo.bcrc.site). Alternatively, try out [builds from GitHub Actions](https://github.com/Swung0x48/BallanceMMO/actions/workflows/client.yml) which also support [BallanceModLoaderPlus](https://github.com/doyaGu/BallanceModLoaderPlus).

3. Put `BallanceMMO_x.y.z-[stage]b.bmod` and required `.dll` dependencies under `$(game_directory)\ModLoader\Mods`. (If the directory is not present, you may launch the game and BML will generate that for you)

4. Launch the game and enjoy!

### Server

1. Download your desired [server builds from GitHub Actions](https://github.com/Swung0x48/BallanceMMO/actions/workflows/server.yml), as currently we don't have a release page for it (maybe we'll make one in the future?).

2. Extract the archive and run `start_ballancemmo_loop.bat` (Windows) or `start_ballancemmo_loop.sh` (Linux/macOS), which is our helper script to take care of logging and server crashes.

3. Modify the generated `config.yml` according to your needs; you may also run `./BallanceMMOServer --help` to see if there are any configurable command line arguments (you can also supply them through our helper script!).

4. Type `reload` in the server console and apply your config changes. Now you're ready to share your address and invite others!

## Dependencies or build tools

- CMake 3.12 or later: Generate makefiles
- A build tool like GNU Make, Ninja: Build server binary
- A compiler with core C++20 feature support
  - This project has been successfully compiled under:
    - GCC ~~9 to 11,~~ 12.2 to 15.2 (server-side components; GCC 11 support dropped as of August 2025)
    - Apple Clang 13.1, 16.0, and 20.1 (server-side)
    - Visual Studio 2019 and 2022 (client-side, or specifically, BML-related stuff)
- One of the following crypto solutions:
  - OpenSSL 1.1.1 or later
  - OpenSSL 1.1.x, plus ed25519-donna and curve25519-donna. (Valve GNS has made some minor changes, so the source is included in this project.)
  - libsodium
- Google protobuf 2.6.1+ (included in submodules)
- Steam GameNetworkingSockets (included in submodules)
- [Read Evaluate Print Loop ++ (*replxx*)](https://github.com/AmokHuginnsson/replxx) (included in submodules)
- Dev pack of BallanceModLoader (client-side, [*release page*](https://github.com/Gamepiaynmo/BallanceModLoader/releases)) and (optionally) [BallanceModLoaderPlus](https://github.com/doyaGu/BallanceModLoaderPlus)

## Building server

1. Clone this repo __RECURSIVELY__

    ```commandline
    git clone https://github.com/Swung0x48/BallanceMMO.git --recursive
    ```

2. ***(Linux/macOS)*** Install OpenSSL and Protobuf

    - Debian/Ubuntu

        ```commandline
        apt install libssl-dev
        apt install libprotobuf-dev protobuf-compiler
        ```

    - Arch

        ```commandline
        pacman -S openssl
        pacman -S protobuf
        ```

    - macOS with brew

        ```commandline
        brew install openssl@1.1
        export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/usr/local/opt/openssl@1.1/lib/pkgconfig
        brew install protobuf
        ```

    (Haven't tried on a Linux distro with yum as package manager. Sorry Fedora/RHEL/CentOS guys...)

3. Building!

    + **Linux/macOS**
        - Ninja

            ```commandline
            ./build_server.sh
            cd build
            ninja
            ```

        - GNU Make

            ```commandline
            ./build_server.sh -G
            cd build
            make
            ```

        You may also want to supply command line arguments such as `-DCMAKE_BUILD_TYPE=Release` when running `build.sh`, to specify the build type.

        If CMake failed to find openssl for some reason, you may need to specify path to openssl yourself.

        For example:

        ```commandline
        $ ./build_server.sh -DOPENSSL_ROOT_DIR="/usr/local/opt/openssl@1.1/"
        ```
    
    + **Windows**
        
        We're using vcpkg to take care of our dependencies on Windows. It should be automatically called during CMake setup.

        ```commandline
        > .\build_server.bat
        ```

        Open and compile the generated Visual Studio Solution file in `build_server` after finishing this, and you should be fine to go.

4. Run your server!

    (Don't forget to navigate to the build directory first: `cd BallanceMMOServer`)

    ```commandline
    $ ./BallanceMMOServer
    ```

    For help or customization, use the command line argument `--help`.

    ```commandline
    $ ./BallanceMMOServer --help
    Usage: ./BallanceMMOServer [OPTION]...
    Options:
      -p, --port=PORT      Use PORT as the server port instead (default: 26676).
      -l, --log=PATH       Write log to the file at PATH in addition to stdout.
      -h, --help           Display this help and exit.
      -v, --version        Display version information and exit.
          --dry-run        Test the server by starting it and exiting immediately.
    ```

    Alternatively, use the bash script `start_ballancemmo_loop.sh` (`start_ballancemmo_loop.bat` on Windows) which handles file logging automatically and restarts the server after crashes. *All command line arguments for the server executable also applies there.*

    ```commandline
    $ ./start_ballancemmo_loop.sh
    ```

5. (Linux bonus) Building portable AppImages

    As you may have already seen, our Linux [server builds from GitHub Actions](https://github.com/Swung0x48/BallanceMMO/actions/workflows/server.yml) use AppImages. This is because some system dependencies used by our build outputs differ in version with different systems/distros, and we can pack them in an AppImage file to increase portability.

    To build the AppImage, you have to first re-run the CMake configuration to tell it that you're trying to build one (append the `-G` switch if you want to continue with GNU Make):

    ```commandline
    $ ./build_server.sh -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_SERVER_APPIMAGE=ON
    ```

    Then navigate to the binary output directory (this is to ensure that we can find our dependencies) and **install** the binaries somewhere (e.g. `AppDir`):

    ```commandline
    cd build/BallanceMMOServer
    DESTDIR=AppDir ninja -C .. install
    ```

    (replace `ninja` with `make` if you want)

    We use *linuxdeploy* to build our AppImages ([get its releases here](https://github.com/linuxdeploy/linuxdeploy/releases)), so download and place it in the binary output directory, then run the command to generate our AppImage (if you can't run this, then try using the `--appimage-extract` switch to extract and use `squashfs-root/AppRun` instead):

    ```commandline
    $ ./linuxdeploy.AppImage --appdir ../AppDir --output appimage --desktop-file ../../BallanceMMOServer/appimage/BMMOLaunchSelector.desktop --icon-file ../../BallanceMMOServer/appimage/BallanceMMO.svg
    ```

    If everything goes smoothly then a file named like `BallanceMMOLaunchSelector-*arch*.AppImage` should appear, which bundles required dependencies and includes other tools like *BallanceMMOMockClient* (thus the **LaunchSelector** name). To see its usage:

    ```commandline
    $ ./BallanceMMOLaunchSelector.AppImage --help
    Usage: ./BallanceMMOLaunchSelector.AppImage [OPTION]...
    Options:
    -h, --help              Display this help and exit.
    -l, --launch [Target]   Launch the selected target.
    Additional options will be forwarded to the target.
    Available targets (default: `Server`):
      Server
      MockClient
      RecordParser
    Examples:
      To see the server help:
        ./BallanceMMOLaunchSelector-x86_64.AppImage --launch Server --help
      To launch a mock client with a custom name:
        ./BallanceMMOLaunchSelector-x86_64.AppImage -l MockClient -n Name
    ```

## Building client (Game Mod)

### Windows

On Windows, we can use the vcpkg package manager. The following instructions assume that you will follow the steps and fetch vcpkg as a submodule. If you want to install vcpkg somewhere else, you're on your own.

__Warning Beforehand__: DO NOT upgrade (cancel when prompted) when Visual Studio asks you to upgrade the solution/project on first launch. It will wipe include directories which has been set in the project file (wtf Microsoft???). Don't worry, following steps will guide you to switch to your already-set-up msvc toolchain.

1. Install/Modify Visual Studio and CMake
    - In `Workload` tab, make sure you have installed `Desktop Development with C++`.
    - __IMPORTANT!__ In `Language pack` tab, make sure you have installed `English`.

2. Bootstrap vcpkg
    ```commandline
    cd submodule\vcpkg
    .\bootstrap-vcpkg.bat
    ```

3. Generate the Visual Studio project file with CMake
    - CMake will automatically call vcpkg to take care of our dependecies during the setup.
    - This is the step where vcpkg will fail if you haven't got your English language pack installed. (Again, wtf Microsoft???)

        ```commandline
        > .\build.bat
        ```

4. Extract BML Dev pack and [Boost Library](https://www.boost.org/users/download/), and place the following:
    - *BML*:
        - include\BML -> BallanceMMOClient\include\BML
        - lib\Debug -> BallanceMMOClient\lib\BML\Debug
        - lib\Release -> BallanceMMOClient\lib\BML\Release
    - *Boost*:
        - boost -> BallanceMMOClient\include\boost

5. Open the Visual Studio project file `BallanceMMO.sln`, which could be found in the `build` directory.

6. Build the project!
