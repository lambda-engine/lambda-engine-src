# Lambda Engine (UE5)

A Source-engine-style moddable base game on Unreal Engine 5.8: maps are compiled with Hammer (`.bsp`) and loaded
**at runtime** from a Half-Life-like game directory, with materials from `.vmt`/`.vtf` files (loose or inside VPK
archives). Lighting comes from UE5 dynamic lights (no lightmaps).

## Layout

The engine and the game files are two repositories. This one is the Unreal project; the other holds everything a
player actually runs, and is where packaging puts the built game.

```
lambda-engine-src/            this repository - the Unreal project
  LambdaEngine.uproject
  GameDir.txt                 where the game repository is on this machine (see below)
  Source/LambdaEngine/        game module: ALambdaGameMode, ALambdaCharacter (HL2-style FPS), console commands
  Plugins/LambdaSource/       Source-format runtime: BSP/VMT/VTF/KeyValues/VPK parsers, virtual file system, world actor
  Config/  Content/
  Build/Windows/              Application.ico - the icon compiled into the packaged exe
  Tools/                      dev scripts: GenerateProjectFiles / Build / PlayDev / CreateAssets / Editor / Package

lambda-engine/                the other repository - the shippable game files
  LambdaEngine.exe            the packaged game (created by Tools\Package.bat over here)
  Engine/  LambdaEngine/      cooked engine + game data (created by packaging)
  <map>.bat                   launches the game on Game\lambda\maps\<map>.bsp
  Mods/                       all mod content lives here; packaging never touches it
    lambda/                   the mod content directory (like hl2/)
      gameinfo.txt            declares the mod name and its content mounts (dirs, VPKs, Steam installs, packs)
      maps/  materials/       mod content
      plugins/                content packs, each mounted in turn (like Source's custom/)
```

## GameDir.txt

Because the two are separate, the tools and the editor have to be told where the game files are. `GameDir.txt`
beside the `.uproject` holds one absolute path - your clone of the game repository:

```
C:\Users\you\Development\lambda-engine
```

`Tools\Env.bat` reads it, so `Package.bat` knows where to put the build and `PlayDev.bat` knows what to mount; the
editor reads it too, which is what lets Play work without passing `-gamedir` every time. A `-gamedir=` on the
command line still wins over it, and so does a `GAME_DIR` already set in the environment.

`Tools\PlayDev.bat` runs the code you are currently editing without cooking; `Package.bat` builds the game into
the game repository, where its `.bat` launchers run it.

## Requirements

* Unreal Engine 5.8 (Epic Games Launcher, default `C:\Program Files\Epic Games\UE_5.8`)
* Visual Studio 2026 with "Game development with C++" (MSVC v145 14.50+), or VS 2022 17.14 (MSVC 14.44.35211+)

## Build & run

```bat
src\Tools\GenerateProjectFiles.bat    rem creates src\LambdaEngine.sln
src\Tools\CreateAssets.bat            rem one-time: master material + empty startup level (editor Python)
src\Tools\PlayDev.bat test            rem compile + play from source - the day-to-day dev loop
```

Or open `src\LambdaEngine.uproject` in the editor and press Play.

### Which script runs what

There are two separate builds here, and mixing them up is the easiest mistake to make:

| Command | Builds | Runs |
|---|---|---|
| `src\Tools\Build.bat` | editor DLLs | nothing |
| `src\Tools\PlayDev.bat` | editor DLLs | the game **from source** (no cooking) |
| `src\Tools\Package.bat` | cooked standalone build into `game\` | nothing |
| `game\Run.bat` | nothing | the **packaged** `game\LambdaEngine.exe` |

`Build.bat` does *not* refresh `game\LambdaEngine.exe` — only `Package.bat` does. While iterating on code use
`PlayDev.bat`; repackage when you want the shipped exe to catch up.

## Packaging

```bat
src\Tools\Package.bat                 rem Development build; pass "Shipping" for a shipping build
```

This cooks and stages the game straight into `game\`, so that folder becomes self-contained:
`LambdaEngine.exe` sits next to `Game\`, which packaging never overwrites. Re-run this whenever you want the shipped
exe to pick up code or content changes — a plain `Build.bat` will not.

The exe's icon comes from `src\Build\Windows\Application.ico` (UBT picks it up automatically); replace that file and
repackage to change it.

## Content sources (gameinfo.txt and VPK mounting)

Content mounts are declared in `Mods/lambda/gameinfo.txt`, Source-style — this is the single source of truth:

```
"GameInfo"
{
    "game"  "Lambda Mod"
    "FileSystem"
    {
        "SearchPaths"
        {
            "game"  "|gameinfo_path|."                       // this mod's loose content (highest priority)
            "game"  "<path>/hl2/hl2_misc_dir.vpk"            // HL2 materials/models/scripts
            "game"  "<path>/hl2/hl2_textures_dir.vpk"        // HL2 textures
        }
    }
}
```

Paths are mounted top-to-bottom and the first match wins, so your loose mod files override packaged HL2 content. A
value ending in `.vpk` mounts an archive (point at the `_dir.vpk` of a multi-chunk set — data is streamed from the
`_NNN.vpk` chunks, nothing is extracted); anything else mounts a loose directory. `|gameinfo_path|` expands to the
game directory, and relative paths resolve against it.

The game directory is found automatically — `<root>/Mods/<mod>` where `<root>` is the folder holding the exe when
packaged and `../game` in the editor; any subfolder containing a `gameinfo.txt` is accepted, so the mod folder can be
renamed freely. It can also be forced with `-gamedir=<path>`.

### Command line / console

* `-sourcemap=<name>` — BSP to load (`lambda/maps/<name>.bsp`). Not `-map=` (engine-reserved).
* `-gamedir=<path>` — extra loose content root, searched first.
* console `lambda.map <name>` — load another map; `lambda.maps` — list available maps across all mounts.

## Conventions

* 1 Hammer unit = 1.905 cm (16 units = 1 foot), configurable (`UnitScale`)
* Source (x, y, z) -> UE (x, -y, z) * scale; face normals derived from surfedge winding (not plane+side)
* Player: capsule 32 x 72 units, eye height 64, walk 190 u/s, sprint 320 u/s (Shift), jump 21 units, gravity 600 u/s^2
