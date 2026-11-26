# Mario Tennis 64 Online

An online multiplayer add-on for Mario Tennis: Recompiled.

A native PC port of **Mario Tennis (N64, USA)**, made with [N64Recomp](https://github.com/N64Recomp/N64Recomp)
and the [RT64](https://github.com/rt64/rt64) renderer. The original game code is translated into C and
compiled to a Windows program, so it runs natively instead of through an emulator.

![A widescreen match](docs/screenshot-match.jpg)

![Character Select with Donkey Kong Jr. and Shy Guy](docs/screenshot-select.jpg)

> **No game files are included.** This repository holds only the port's own code, tools and artwork.
> You supply the game yourself, and it and everything generated from it stay on your machine.

## Scope and credits

**The contribution of this fork is limited to the online multiplayer mode for Windows, its launcher, and related tests.** The game is Nintendo's Mario Tennis for Nintendo 64. The PC port is the work of Ieatzcrayonz and the contributors to MarioTennisRecomp.

- **Base PC port** — [MarioTennisRecomp by Ieatzcrayonz](https://github.com/Ieatzcrayonz/MarioTennisRecomp).
- **Online networking** — [recomp-net by RetroPortingToolKit](https://github.com/RetroPortingToolKit/recomp-net), used for synchronized controller input over UDP.
- **Recompilation and runtime** — [N64Recomp](https://github.com/N64Recomp/N64Recomp) and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), inherited from the port.
- **Rendering and build dependencies** — [RT64](https://github.com/rt64/rt64) and the pinned [Zelda64Recomp dependency tree](https://github.com/Zelda64Recomp/Zelda64Recomp), as used by the original port.

## Online singles and doubles preview for Windows

The launcher offers **1v1 on two PCs**, **2v2 on two PCs** and **2v2 on four PCs**. Each participant uses the same release and their own local USA ROM. For two-PC doubles, the host uses ports 1 and 2 and the guest uses ports 3 and 4. For four-PC doubles, the host uses port 1 and each guest chooses a different seat from 2 to 4.

Automated tests replayed a point in singles and doubles with two and four instances on one Windows PC. Testing on separate physical PCs remains open. [Build instructions and pinned dependency revisions](docs/NETPLAY_BUILD.md).

### Resumo em português

**A contribuição deste fork se limita ao modo online para Windows, ao launcher e aos testes.** O jogo é da Nintendo e o port para PC é o MarioTennisRecomp de Ieatzcrayonz e seus colaboradores. O online usa o recomp-net da RetroPortingToolKit. O port usa N64Recomp, N64ModernRuntime e RT64, com dependências reunidas pelo Zelda64Recomp.

O launcher oferece 1v1 em dois PCs e 2v2 em dois ou quatro PCs. Cada pessoa usa sua cópia local da ROM USA.

## Which ROM to use
Use a dump of your own **Mario Tennis** cartridge, **North American (USA) release**. Other regions
(Europe, Japan) won't work.

| | |
|---|---|
| Name | `Mario Tennis (USA).z64` |
| Game code | `NM8E` |
| Format | `.z64` (big-endian). Convert `.n64` or `.v64` files to `.z64` first, e.g. with Tool64 |
| SHA-1 | `999047f07cec931ffbdcc7b33b8502ef602807ee` |

To check your file on Windows, run this in PowerShell and compare the result with the SHA-1 above:

```powershell
Get-FileHash "Mario Tennis (USA).z64" -Algorithm SHA1
```

If it doesn't match, the port won't recognise the file. The usual cause is the wrong region or a modified (hacked or translated) ROM.

## Download
Get `MarioTennisRecompiled-Windows.zip` from the [Releases](https://github.com/Ieatzcrayonz/MarioTennisRecomp/releases) page,
unzip it, run `MarioTennisRecompiled.exe`, pick your ROM and press Start game. The download contains no game
code or data: the game is translated from your own ROM when it starts.

## Features
- Runs at any resolution: 720p, 1080p, 1440p, 4K or matching the window.
- Widescreen: Aspect Ratio **Expand** makes the game draw a wider 3D view matching your window (16:9,
  ultrawide and so on); menus and the score display are stretched to fit. The opening airship flyover stays 4:3.
- Controller support (including DualSense), with rumble and remappable controls.
- Local multiplayer for up to 4 players: each controller is its own player, in the order they were connected.
  "On + Keyboard" also makes the keyboard a player, so one controller and a keyboard can play 2-player
  (Settings Ã¢â€ â€™ General Ã¢â€ â€™ Local Multiplayer).
- Saves work as on the cartridge.
- Bonus characters: Donkey Kong Jr. and Shy Guy are unlocked from the start.
- Transfer Pak characters: Alex, Harry, Nina and Kate from the Game Boy Color version are on Character Select without a Transfer Pak (level 25, starting stats).
- Optional cheats, all off by default (Settings Ã¢â€ â€™ General): **All Courts**, **Star Characters**,
  **All Tournaments Cleared** and **Infinite Bounces**. The first three are saved with your game
  like normal unlocks, so they stay unlocked after you switch the cheat off.
- A new sunny court launcher screen.
- DirectX 12 or Vulkan (Settings Ã¢â€ â€™ Graphics Ã¢â€ â€™ Graphics API).
- Optional CRT shaders from RetroArch (CRT-Royale, CRT-Royale Fast, CRT-Lottes) via librashader
  (Settings Ã¢â€ â€™ Graphics Ã¢â€ â€™ Shader).

## Building (Windows)
Requirements: Visual Studio 2022+ Build Tools, LLVM/clang-cl, CMake, Ninja, Python 3 and Git Bash.

1. Clone [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) with `--recursive` and make a
   `lib` link in this folder pointing at its `lib` folder (it provides RT64, RmlUi, N64ModernRuntime and friends).
2. Apply the patches in `patches/` to `lib/N64ModernRuntime`, `lib/N64ModernRuntime/N64Recomp` and `lib/rt64`.
3. Generate the recompiled code from your ROM: `python tools/gen_symbols.py "<path to Mario Tennis (USA).z64>"`,
   then `sh tools/recompile.sh`.
4. Build: `tools\build_game.bat` (edit the Visual Studio path in it if yours differs). The result is
   `build\MarioTennisRecompiled.exe`. Run it with this folder as the working directory.
5. Release build: `tools\build_live.bat` (output in `build-live\`). It needs no generated code, so step 3 can
   be skipped: the exe translates the game's code, and runs its audio microcode, from the player's ROM at startup.
6. Optional, for the Shader option: build `librashader.dll` from
   [librashader](https://github.com/SnowflakePowered/librashader) (tag `librashader-v0.12.1`) with
   `cargo xtask build --profile release`, and configure with `-DLIBRASHADER_DLL=<path>\librashader.dll` so it is
   copied next to the exe (`tools\build_live.bat` does this for `F:\Recomps\src\librashader`). The shader presets
   in `slang-shaders/` are copied automatically. Without the DLL the game runs normally and the option does nothing.

Technical notes on how the port works are in [NOTES.md](NOTES.md).

## Credits
- N64Recomp, N64ModernRuntime and the Zelda64Recomp frontend, by Wiseguy and contributors (GPL-3.0).
- RT64 by DarÃƒÂ­o and contributors.
- [librashader](https://github.com/SnowflakePowered/librashader) by Ronny Chan (MPL-2.0 / GPL-3.0).
- CRT-Royale by TroggleMonkey (GPL-2.0+), CRT-Lottes by Timothy Lottes (public domain), from the libretro
  [slang-shaders](https://github.com/libretro/slang-shaders) collection. See [slang-shaders/README.md](slang-shaders/README.md).
- Luckiest Guy font by Astigmatic (Apache 2.0).

Mario Tennis is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo.

## License
GPL-3.0, the same as the Zelda64Recomp code it builds on. See [LICENSE](LICENSE).
