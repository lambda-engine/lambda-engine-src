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
  Source/LambdaEngine/        game module: ALambdaGameMode, ALambdaCharacter (HL2-style FPS), weapons, HUD, menu
  Plugins/LambdaSource/       the Source-format runtime, in folders named after what they do:
    Core/                     module, settings, stats, unit/axis conversion
    FileSystem/               the virtual file system and its VPK archives
    Formats/                  BSP, MDL, VTF, PHY, WAV, KeyValues readers
    Materials/  Audio/        VMT -> material, soundscripts, surface properties, decal scripts
    World/                    the BSP world actor, geometry building, entity parsing and I/O
    Entities/  Creatures/     brush and point entities; the NPCs
    Weapons/  Gameplay/       weapon scripts, ammo table, damage, view punch
    Rendering/                studio models, runtime skeletal meshes, ragdolls, decals, particles
  Config/  Content/
  Build/Windows/              Application.ico - the icon compiled into the packaged exe
  Tools/                      dev scripts: GenerateProjectFiles / Build / CreateAssets / Editor / Env

lambda-engine/                the other repository - the shippable game files
  lambda.exe                  the packaged game (created by Release.bat over here)
  Engine/  LambdaEngine/      cooked engine + game data (created by packaging)
  <map>.bat                   launches the game on Mods\lambda\maps\<map>.bsp
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

`Tools\Env.bat` reads it, so `Release.bat` knows where to put the build and `Debug.bat` knows what to mount; the
editor reads it too, which is what lets Play work without passing `-gamedir` every time. A `-gamedir=` on the
command line still wins over it, and so does a `GAME_DIR` already set in the environment.

`Debug.bat` runs the code you are currently editing without cooking; `Release.bat` builds the game into
the game repository, where its `.bat` launchers run it.

## Requirements

* Unreal Engine 5.8 (Epic Games Launcher, default `C:\Program Files\Epic Games\UE_5.8`)
* Visual Studio 2026 with "Game development with C++" (MSVC v145 14.50+), or VS 2022 17.14 (MSVC 14.44.35211+)

## Build & run

```bat
src\Tools\GenerateProjectFiles.bat    rem creates src\LambdaEngine.sln
src\Tools\CreateAssets.bat            rem one-time: master material + empty startup level (editor Python)
Debug.bat startup            rem compile + play from source - the day-to-day dev loop
```

Or open `src\LambdaEngine.uproject` in the editor and press Play.

### Which script runs what

There are two separate builds here, and mixing them up is the easiest mistake to make:

| Command | Builds | Runs |
|---|---|---|
| `src\Tools\Build.bat` | editor DLLs | nothing |
| `Debug.bat` | editor DLLs | the game **from source** (no cooking) |
| `Release.bat` | cooked standalone build into the game repository | nothing |
| a per-map  | nothing | the **packaged**  |

`Build.bat` does *not* refresh `lambda.exe` — only `Release.bat` does. While iterating on code use
`Debug.bat`; repackage when you want the shipped exe to catch up.

## Packaging

```bat
Release.bat                 rem Development build; pass "Shipping" for a shipping build
```

This cooks and stages the game straight into `game\`, so that folder becomes self-contained:
`lambda.exe` sits next to `Mods\`, which packaging never overwrites. Re-run this whenever you want the shipped
exe to pick up code or content changes — a plain `Build.bat` will not.

The exe's icon comes from `src\Build\Windows\Application.ico` (UBT picks it up automatically); replace that file and
repackage to change it.

## Content sources

Where content comes from and how mounting works is documented in the game repository, next to the `gameinfo.txt`
that declares it — see `lambda-engine/README.md`.

### Command line / console

* `-sourcemap=<name>` — BSP to load (`lambda/maps/<name>.bsp`). Not `-map=` (engine-reserved).
* `-gamedir=<path>` — extra loose content root, searched first.
* console `map <name>` — load another map; `maps` — list available maps across all mounts. The full command
  list is in the game repository's README, and any of them can be given on the command line with `+` in front.

## What is implemented

Where something is a port, the Source file it came from is named beside it. "Generic" means the thing loads and
behaves like everything else of its kind, but has nothing of its own yet.

### Weapons

| Weapon | Status | Notes |
|---|---|---|
| `weapon_crowbar` | Done | `weapon_crowbar.cpp`, `basebludgeonweapon.cpp` — ray then hull, melee shove |
| `weapon_pistol` | Done | `weapon_pistol.cpp` — accuracy penalty per shot, dry-fire refire |
| `weapon_smg1` | Primary only | `weapon_smg1.cpp` on `basehlcombatweapon.cpp` — 13.3hz, recoil ladder. Secondary is the grenade launcher, which needs a grenade |
| `weapon_shotgun` | Done | `weapon_shotgun.cpp` — pump between shots, shell-at-a-time interruptible reload, both barrels on secondary |
| anything else | Generic | any `scripts/weapon_*.txt` loads and fires on the base `CBaseCombatWeapon` behaviour |

Clip sizes, ammo types, sounds and damage come from `scripts/weapon_*.txt`, `scripts/game_sounds_*.txt` and
`cfg/skill.cfg`; only the constants that live in a weapon's own `.cpp` are in the engine.

### NPCs

| Entity | Status | Notes |
|---|---|---|
| `npc_headcrab` | Done | jump attack, ragdoll on death |
| `npc_zombie` | Done | melee, torso/legs gibbing, slump |
| `npc_barnacle` | Done | tongue, lift, swallow, and it will take a prop out of the player's hands |
| `npc_antlion` | Core | chases, swipes and pounces. Burrowing, bugbait, workers, squads and flipping are not ported |

### Point entities

| Entity | Status |
|---|---|
| `info_player_start`, `info_player_deathmatch`, `info_player_coop`, `info_player_terrorist`, `info_player_counterterrorist` | Done |
| `light`, `light_spot`, `light_environment` | Done |
| `prop_physics`, `prop_physics_override`, `prop_physics_multiplayer`, `physics_prop` | Done |
| `point_template` | Done |
| `item_ammo_*`, `item_box_*`, `item_large_box_*`, `item_rpg_round`, `item_ar2_grenade` | Done |
| `weapon_*` lying in a map | Done — gives the weapon its script describes |
| anything else | Counted and logged as unhandled at the end of the load |

### Brush entities

| Entity | Status |
|---|---|
| `func_button` | Done |
| `func_door_rotating` | Done |
| any other `"model" "*N"` entity | Geometry only — it is drawn and collides, but does nothing |

### World

| | Status |
|---|---|
| BSP v19–21 geometry, collision, brush models | Done |
| Entity I/O (`ent_fire`, outputs, `!activator`/`!caller`) | Partial — `Use` |
| Displacements | Not started — the faces are counted and skipped |
| Static props (game lump) | Not started |
| Water, 3D skybox, overlays | Not started |
| Lightmaps | Not planned — lighting is UE5 dynamic lights |

### Formats read

| | Status |
|---|---|
| BSP (v19–21), KeyValues, VPK | Done |
| VMT, VTF (7.0–7.5, DXT1/3/5 and the uncompressed formats) | Done |
| MDL/VVD/VTX (v44+), PHY ledge trees | Done |
| WAV, soundscripts (`game_sounds_*.txt`), `surfaceproperties`, `decals_subrect` | Done |

### Systems

| | Status |
|---|---|
| Runtime skeletal meshes, GPU skinning, bodygroups | Done |
| Impact decals, blood, ragdolls, particles | Done |
| HUD (health, suit, ammo), weapon selection | Done |
| Main menu, pause menu, developer console, loading screen | Done — `map` is the only console command that does anything of its own |
| Save / load | Not started |
| Options, achievements | Not started — the menu entries say so when picked |

## Conventions

* 1 Hammer unit = 1.905 cm (16 units = 1 foot), configurable (`UnitScale`)
* Source (x, y, z) -> UE (x, -y, z) * scale; face normals derived from surfedge winding (not plane+side)
* Player: capsule 32 x 72 units, eye height 64, walk 190 u/s, sprint 320 u/s (Shift), jump 21 units, gravity 600 u/s^2
