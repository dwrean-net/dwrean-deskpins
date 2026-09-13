# dwrean DeskPins

**dwrean DeskPins** is a modernized fork of the classic DeskPins utility for Windows. It lets you keep any normal application window permanently above the others with a quick click or keyboard shortcut.

The goal of this fork is to preserve the simplicity of the original application while making the project easy to build, maintain and improve on current Windows versions.

## Modern version

The new implementation lives in `src/modern` and is intentionally dependency-free. It uses the native Win32 API and does **not** require Boost or the original private `eflib` framework.

Current modern features:

- Pin or unpin a window from the system tray.
- `Ctrl+Alt+P` toggles the currently active window.
- Tray menu with a list of pinned windows.
- Unpin individual windows or all pinned windows at once.
- Optional **Start with Windows** setting.
- Single-instance protection.
- Multi-monitor window selection.
- Unicode build for current Windows versions.
- Automatic x64 and x86 builds with GitHub Actions.

## Building

The modern version requires:

- Windows 10 or Windows 11
- Visual Studio 2022 with the Desktop development with C++ workload
- CMake 3.21 or newer

Example:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable will be created as:

```text
build/Release/dwrean-deskpins.exe
```

GitHub Actions also builds portable x64 and x86 executables automatically.

## Legacy DeskPins source

The original DeskPins source is intentionally kept in this repository for reference and attribution. That code was originally created for older Visual Studio versions and depends on Boost and `eflib`.

The modern CMake target builds only the new implementation in `src/modern`.

## Credits

Original DeskPins by **Elias Fotinis**.

Modernized fork maintained by **dwrean.net**.

## License

This project continues to use the original MIT license. See `license.txt` for the full license text and original copyright notice.
