# Build, install and package

[Back to Rung](../README.md)

## Supported platforms

| Platform | Interpreters | Native JIT |
|---|---|---|
| macOS on Apple Silicon | Tree, stack, register | Yes, with NaN-boxing |
| Linux ARM64 | Tree, stack, register | Yes, with NaN-boxing |
| Linux x86-64 | Tree, stack, register | No |

Windows users can use Ubuntu in WSL2 or Docker for the interpreters. Native Windows builds are
not supported. The JIT and its background compilation mode both require ARM64.

## Prerequisites

CMake 3.25+, Ninja, Clang with C++20, Python 3.9+, and Git. The C++ runtime has no external
dependencies; doctest is vendored. Pillow is needed only to regenerate demo images.

On macOS, install the Xcode Command Line Tools (`xcode-select --install`) if needed, then with
Homebrew:

```sh
brew install cmake ninja python
```

On Ubuntu 24.04:

```sh
sudo apt-get update
sudo apt-get install -y clang cmake ninja-build python3 git
```

## Build and try it

```sh
git clone https://github.com/aadityad12/rung.git
cd rung
cmake --preset debug
cmake --build --preset debug
./build/debug/rung examples/fib.rg   # 6765
python3 scripts/demo.py --rung build/debug/rung
ctest --preset debug
```

The Debug tour runs three interpreters and explicitly skips the unavailable JIT. To run every
part on ARM64:

```sh
cmake --preset release-goto-nanbox
cmake --build --preset release-goto-nanbox
python3 scripts/demo.py --rung build/release-goto-nanbox/rung
ctest --preset release-goto-nanbox
```

Use `rung --help` for usage and `rung --version` to see the version and engines in your build.
Relative program paths are resolved from the directory where you run the command.

## Install under your home directory

```sh
cmake --install build/release-goto-nanbox --prefix "$HOME/.local"
"$HOME/.local/bin/rung" --version
python3 "$HOME/.local/share/rung/scripts/demo.py" --rung "$HOME/.local/bin/rung"
```

For a Debug build, substitute `build/debug`. The install contains the binary, examples,
documentation and guided tour. You can put `$HOME/.local/bin` on your `PATH` to use `rung`
without its full path.

## Docker

The existing container builds the release interpreters and runs their tests by default:

```sh
docker build -t rung .
docker run --rm rung
docker run --rm rung python3 scripts/demo.py --rung build/release/rung
docker run --rm rung ./build/release/rung examples/fib.rg
```

This image does not enable the JIT, even on ARM64. Use a native ARM64 NaN-boxed build for the
JIT demonstrations. Docker is for trying the language and checking correctness, not measuring
performance.

## Portable archives

Create a versioned archive of the build on your machine:

```sh
cpack --config build/release-goto-nanbox/CPackConfig.cmake -B build/packages
```

Archives are named `rung-<version>-<platform>-<architecture>.tar.gz`. Extract the matching
archive, enter its directory, then run:

```sh
./bin/rung --version
./bin/rung share/rung/examples/fib.rg
python3 share/rung/scripts/demo.py --rung bin/rung
```

No compiler or CMake is needed to use an archive. Python is needed only for the guided tour.
Linux archives from the release workflow target Ubuntu 24.04 or newer; macOS archives target
macOS 15 or newer. The archives are not signed or notarized.

The [packages workflow](../.github/workflows/packages.yml) builds and tests native archives for
macOS ARM64, Linux ARM64 and Linux x86-64, then smoke-tests the extracted packages. Manual runs
upload archives and SHA-256 checksums as workflow artifacts. A version tag matching the CMake
version (for example `v0.1.0`) prepares a **draft** GitHub release with the same files. Published
downloads will appear on the [Releases page](https://github.com/aadityad12/rung/releases) once a
release has been reviewed and published.
