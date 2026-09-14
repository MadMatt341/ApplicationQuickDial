# Release workflow

Read when preparing downloadable assets or publishing a version. [Build setup](development.md#prerequisites) · [Test suites](testing.md#targets-and-integration-commands)

## Public release package

Run `./package.ps1 -Version <new-version>` from a shell with CMake and CTest available. It defaults to Visual Studio 2022; pass `-Generator "Visual Studio 18 2026"` for Visual Studio 2026. It configures a separate `build/package` directory, builds Release, runs the CTest suite, and packages only the executable, README, and MIT license under `build/releases`. It also writes a SHA-256 checksum beside the ZIP. Existing versioned ZIPs are never overwritten. Review and commit the source and documentation before publishing the matching tag and assets.

All targets statically link the MSVC runtime (`/MT`, or `/MTd` for Debug), so the portable executable does not require a separate Visual C++ Redistributable installation.

Before packaging, choose an unused version and update `project(... VERSION ...)` in `CMakeLists.txt`. Run affected desktop integration suites against `build/package/Release` as well as the CTest checks performed by the script. Inspect the ZIP contents and compare its executable hash with the tested build. Commit the release source and tag that exact commit. Upload the ZIP and checksum, verify uploaded digests, then publish the release as latest. Pushing source to `main` alone does not update downloadable assets.
