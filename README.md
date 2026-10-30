# paa-viewer

View Arma / DayZ `.paa` and `.pac` textures in Windows Explorer and XnView MP.

- **Windows**: thumbnails, preview pane and photo viewers, through a WIC codec.
- **XnView MP**: a format plugin, to browse `.paa` files next to `.dds`, `.png`...
- All PAA types: DXT1–DXT5 (LZO), ARGB4444/1555/8888 and AI88 (LZSS). Channel swizzles are undone like TexView does.

The decoder is written from the public format description ([docs/FORMAT.md](docs/FORMAT.md)) and has no dependencies.

## Install

Releases come as two downloads; use one or both.

- **Windows Explorer** (`paa-viewer-*-win64.zip`): run `install.bat` as administrator, then `clear-thumbnails.bat`
  if old icons remain. Uninstall with `uninstall.bat`.
- **XnView MP plugin** (`paa-viewer-xnview-*-win64.zip`): close XnView MP and run `install-xnview.bat` as
  administrator. Uninstall with `uninstall-xnview.bat`.

If Microsoft Defender ASR rules block them, allow `C:\Program Files\PaaViewer\PaaWic.dll` or
`C:\Program Files\XnViewMP\Plugins\Xpaa.usr` with `Add-MpPreference -AttackSurfaceReductionOnlyExclusions`.

## Build

Needs CMake 3.20+ and Visual Studio 2022 (C++). Run `build.bat`: the package is created in `dist\`.

`paa2png` (export to PNG) and `wictest` (decode through the codec) are built too.

## License

MIT. Not affiliated with Bohemia Interactive.
