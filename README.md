# fnxf_xiii
Static Recompilation of FINAL FANTASY XIII for Nintendo Switch

No game files are in this repository.

If you want to contribute, you can send me `fnxf_xiii_variants.txt`, `fnxf_xiii_uam_paths.txt`  `pipeline_list.txt` from `fnxf_xiii/shaders` folder after playing some part of game, the best would be if you would send it after finishing whole game.

## Additions over the Xbox 360 release

A settings menu at the first start (smash A button after launching homebrew to show it again), with separate graphics settings for handheld and docked:

- Language: English, German, French, Spanish, Italian, and the language packs in `/switch/fnxf_xiii/lang` (below)
- Frame interval: 1 to 4 display refreshes a frame (2, the default, is 30 FPS; 1 is 60 FPS at 60 Hz)
- Resolution of the 3D scene: 1024x576 (the game's own, the default), 1280x720, 1600x900, 1920x1080
- Anti-aliasing: off, MSAA 2x (the game's own, the default), MSAA 4x, MSAA 8x or FSR 3.1
- Upscaler: Lanczos (the default) or FSR 1
- Dynamic resolution
- The list of achievements

Additional improvements:
- A popup when an achievement is unlocked
- 1920x1080 output when docked on a 1080p TV
- The Switch's buttons in the game's interface
- Removed two first popups about selecting user and mass storage device, removed mass storage device option from save menu

## Required game files

The Xbox 360 release with Title ID `535107E4` and Media ID `602A3E5A`.

Its texts are in English, German, French, Spanish and Italian. More languages through language packs, more below.

Skip to "Game data" section if you downloaded NRO from Releases.

## Tools (Windows)
```
    winget install Kitware.CMake
    winget install Ninja-build.Ninja
    winget install Python.Python.3.14
    winget install LLVM.LLVM --version 20.1.0
    winget install Microsoft.VisualStudio.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Git (`winget install Git.Git`) is needed only by `build_ppc_tests.bat`.

devkitPro is not in winget. Install it with its installer (https://github.com/devkitPro/installer/releases,
"Switch Development"), then in the devkitPro MSYS2 shell:

    pacman -S switch-dev switch-zlib switch-zstd

Set `DEVKITPRO` to devkitPro's folder in Windows `PATH` variables.

## Building

1. Put `default.xex` from Disc 1 into `app/assets/`.
2. Run `build_switch_batched.bat`. The result is the folder `out/fnxf_xiii`.
3. Copy `fnxf_xiii` to the SD card's `/switch` and from root of repository copy `shaders` folder to `fnxf_xiii`.

## Game data

On the SD card, into `/switch/fnxf_xiii/assets`:

1. Copy the contents of Disc 1.
2. Copy the contents of Disc 2, skipping every file that is already there under the same name.
2. Copy the contents of Disc 3, skipping every file that is already there under the same name.

The first start compiles the game's shaders on the console.

## Language packs

The port can show the game in a language the Xbox 360 release does not have. A language pack is one zip file in `/switch/fnxf_xiii/lang` on the SD card (for example `lang/pl.zip`). Every pack there is listed in the settings menu's Language row. The pack replaces the game's texts, any text the pack does not have come from its base language. Nothing of the game's files is changed: the pack is applied as the game reads them.

What a pack holds (only `language.ini` is required):

- `language.ini`: the language's name as the menu shows it (`name`), the game's language under it (`base`: english, german, french, spanish or italian), `credits`, and in `[port]` the port's own texts (settings menu, shader compilation screen) in that language
- `texts/`: the game's texts
- `glyphs/`: letters the game's fonts lack, as pictures (`glyphs.ini`: which character each one replaces, a PNG per font size)
- `images/`: translated pictures (location titles, tutorials, the title logo...) in place of the game's textures (`images.ini`: which texture each PNG replaces, by a hash of its data)
- `layouts/`: changes to the interface's layouts, where a translated picture needs more room (`layouts.ini`)
- `achievements.toml`: the achievements' names and descriptions for the achievement popup and the settings menu

The tools in `tools/` make the parts of a pack from the game's files (extracted from the game's archives, for example with WhiteBinTools): `ztr_to_texts.py` (texts), `font_export.py` (the fonts' letters, to draw new ones), `texture_export.py` (textures as PNGs, with the lines of `images.ini`), `lang_image_parts.py` (a translated picture cut down to the parts that changed) and `layout_diff.py` (a translation's layout changes). Each one describes its use at its top. They were done based on Polish fantranslation for PS3 release, other fantranslations may have used different approach that will need additional code to support it.

## Included language packs

- Polish -  made from the fan translation by the CosmoFF group ([www.balamb.pl](https://www.balamb.pl)): Robin and Em, with technical help from Albeoris and Jindrich "Kruci" Tomek ([rk-translations.cz](https://www.rk-translations.cz)), made with FluffyQuack's import/export tool. It is converted from their PlayStation 3 release 1.0.1 (6 January 2022): texts, the Polish letters of the fonts, translated pictures and layouts. The achievements, boot menu and shader compiler texts were made by Claude. The pack is in this repository with the translators' permission. Support the translators:<br>[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://buycoffee.to/ogrodbalamb)  [![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg)](https://paypal.me/balambpl)

## Best performance

1. Build with `build_switch_batched.bat`, start it on the console and let the shader pre-pass finish.
2. Build with `pgo_generate_switch_batched.bat` and start `fnxf_xiii_pgo.nro` on the console.
3. Play game in this order:
    1. Skip intro video
    2. Start new game
    3. Wait for video to go past "Final Fantasy XIII" logo, skip video.
    4. Play whole cutscene, fight enemy, play whole cutscene, fight enemy, play whole cutscene.
    5. Move forward, engage in fight with first enemy but don't fight, your companion will win anyway.
    6. Move forward, engage in fight with next enemy and fight normally.
    7. After getting back from second battle, close game. 
4. Copy the `run_*.fxpg` files from the SD card's `/switch/fnxf_xiii/pgo` to `pgo/` in the project's root.
5. Build with `pgo_use_switch_o3_lto_batched.bat`. The final NRO is `out/fnxf_xiii/fnxf_xiii_o3_lto.nro`.

## Third-party code

- [reNut](https://github.com/masterspike52/reNut): the project this one started from, and its renderer
- [renut_nx_ultimate_base](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base): optimization passes
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (`sdk/` is a fork of it), itself derived from [Xenia](https://github.com/xenia-project/xenia)
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (reblue's fork): shader translation (modified version is stored in this repository)
- [plume](https://github.com/renderbag/plume): the rendering back end (modified version is stored in this repository)
- [uam](https://github.com/devkitPro/uam), with Mesa's GLSL compiler: the deko3d shader compiler (modified version is stored in this repository)
- AMD FidelityFX Super Resolution 3.1 (modded version) and 1.0
- PowerPC instruction tests of [Xenia Canary](https://github.com/xenia-canary/xenia-canary)
- FFmpeg, fmt, spdlog, toml++, inja, nlohmann/json, CLI11, SIMDe, libmspack, o1heap, xxHash, zstd, stb, utfcpp,
  tiny-AES-c, aes_128, smol-v, volk, Vulkan-Headers, VulkanMemoryAllocator
- devkitPro's devkitA64, libnx and deko3d (not in this repository)

Each keeps its own licence in its folder.

## Thanks

- ChanseyIsTheBest for helping with first steps into porting this game
- masterspike52 for reNut
- Tom Clay and the ReXGlue contributors
- the Xenia and Xenia Canary teams
- hedge-dev for XenonRecomp and XenosRecomp
- renderbag for plume
- fincs for deko3d and uam
- devkitPro and switchbrew for devkitA64 and libnx
- StevenSND for the Horizon layer of the ReXGlue fork
- AMD for FidelityFX Super Resolution
- the CosmoFF group for the Polish translation
- Claude for porting this
