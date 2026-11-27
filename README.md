<p align="center">
  <img src="assets/logo.png" alt="Sunset Racer 2 Modernized logo" width="160" />
</p>

<h1 align="center">Sunset Racer 2</h1>

<p align="center"><em>Sunset Racer 2 Story Mode with widescreen 1080p, smooth motion and a portable Windows build.</em></p>

<p align="center">
  <a href="https://github.com/SirBarnaby/Sunset-Racer-2-Modernized/releases/latest/download/Sunset-Racer-2-Windows.zip"><img src="https://img.shields.io/badge/download-Windows%20ZIP-24292f?style=flat" alt="Download the Windows ZIP" /></a>
  &nbsp;
  <img src="https://img.shields.io/badge/display-1080p-24292f?style=flat" alt="1080p display" />
</p>

<p align="center">
  <img src="assets/night-race.gif" alt="A night race through the city in the fog" width="100%" />
</p>

This is a standalone Windows rebuild of Inludo’s Sunset Racer 2 Story Mode (build 376). It keeps the original racing and adds widescreen rendering, smoother motion and optional visual effects.

<p align="center"><sub><a href="#features">Features</a> · <a href="#screenshots">Screenshots</a> · <a href="#install">Install</a> · <a href="#performance">Performance</a> · <a href="#credits">Credits</a></sub></p>

## Features

- **Widescreen and 1080p.** Use borderless fullscreen at your monitor’s resolution or resize the window. Menus and the HUD scale separately. Press <kbd>Alt</kbd>+<kbd>Enter</kbd> to switch display modes.
- **Smooth racing.** The original 30 Hz race simulation is interpolated between updates. Set a frame rate cap or run uncapped in *Options → Display settings*.
- **DirectX 9 and Vulkan.** Choose a renderer in *Options → Visual effects*. The change takes effect when you enter the next race.
- **Optional effects and 3D audio.** Tune anti-aliasing, ambient occlusion, motion blur, lighting, weather and Steam Audio reverb in *Options → Visual effects*.

## Screenshots

<p align="center">
  <img src="assets/main-menu.jpg" alt="Sunset Racer 2 main menu at 1920 × 1080" width="49%" />
  <img src="assets/race-go.jpg" alt="The start of a race at night" width="49%" />
</p>

## Install

1. Download [Sunset-Racer-2-Windows.zip](https://github.com/SirBarnaby/Sunset-Racer-2-Modernized/releases/latest/download/Sunset-Racer-2-Windows.zip).
2. Extract the whole ZIP to a folder.
3. Open the extracted `Sunset Racer 2` folder and run `SunsetRacer2.exe`. Keep the EXE alongside the included files and folders.

## Performance

The Director renderer is single-threaded. In project testing, the build held 60 FPS at 1920 × 1080 with anti-aliasing enabled on an Intel Core i7-8550U and NVIDIA Quadro P500. On a 60 Hz display, select a 60 FPS cap in *Options → Display settings* for steadier motion.

## Credits

Original game by Inludo. The Windows ZIP includes the DXVK and Steam Audio notices in `licenses/`. [Inludo’s Sunset Racer 2 page](https://inludo-game-development.itch.io/sunset-racer-2)
