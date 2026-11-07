# Ocarina ultrawide add-on

`Ultrawide (built-in)` is a game Add-On for Ocarina of Time. It is off by default. It applies
only to title `0400486024DEC000`, module `main`, build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`, and only while **Graphics → Aspect Ratio** is
**Force 21:9**. With any other Aspect Ratio it logs `Skipping "Ultrawide (built-in)": Aspect Ratio is not R21_9` and the game stays stock.

To use it: right-click the game → **Properties → Add-Ons**, tick **Ultrawide (built-in)**, set
**Graphics → Aspect Ratio → Force 21:9**, and restart the game.

## Credit and source

The hooks and routines are a port of [craftycorvid/oot-mod](https://github.com/craftycorvid/oot-mod)
at `c5ccd1cd57e8219aea3be1e758221795fa5b7ec8`, MIT License, Copyright (c) 2026 Ivan0xFF. Its
`build.py` is ported to the constexpr generator in `src/core/file_sys/ocarina_ultrawide.h`, which keeps
that file's MIT terms (`LICENSES/MIT.txt`). A unit test checks that the generator reproduces the
released `oot-ultrawide-21x9-2560x1080` patch word for word.

## What it changes

The game still renders a 16:9 frame. The patch changes what is in it, and the emulator's 21:9 layout
stretches the frame to 21:9, so the result is a wider view, not a stretched one.

| Hook (`main+`) | Change |
| --- | --- |
| `0x3485CC0` | `sead::PerspectiveProjection::doUpdateMatrix`: an aspect within 0.01 of 16:9 becomes the target and is written back to `mAspect`, so culling sees it too |
| `0x004F1E8`, `0x023B9A8`, `0x033B82C` | Inlined copies of the same matrix builder |
| `0x025D76C` | Render view setup: the w/h aspect passed to shaders gets the same mapping |
| `0x0774368` | 2D canvas: the full-screen 3840x2160 layout ortho is squeezed to the centre 16:9, except the item, pause and system menu backgrounds, which redraw a capture of the whole frame |
| `0x3B5C758` | Pre-rendered movies get a centred 16:9 viewport |

The routines sit in the zero executable padding after `.text`, at `0x3C46400` to `0x3C465E3`.
Dynamic FPS uses `0x3C46200` to `0x3C46277` and Max Resolution patches `0x805BD0`, so all three
add-ons can be enabled together.

## Guards

The loader's built-in patch checks apply: build ID, every original hook word, that the cave is still
zero-filled executable padding, and that no user mod has already touched any record. The patch is
applied as a whole or not at all. The seven original words were also checked against a locally
extracted image of the supported `main`.

## Default-off add-ons

`disabled_addons` lists add-ons switched away from their default. Every existing add-on defaults to
on, so nothing changes for them. A default-off built-in is listed once the user enables it.
`FileSys::FlipDefaultOffAddons` converts between that stored list and a plain disabled list at the
boundaries: PatchManager, the Qt Add-Ons page and the Android JNI.

## Known limits

- HUD, full-screen menus and fades cover only the centre 16:9; the 3D world shows at the sides.
- The frame is still rendered at 16:9 and stretched by 21/16 horizontally. At Max Resolution
  (3840x2160 internal) that is still at least one source pixel per screen pixel on a 3440x1440 screen.
- Force 21:9 is exactly 21:9 (2.333). On a 64:27 or 43:18 monitor the layout adds thin side bars;
  geometry stays correct because the patch and the layout use the same ratio.

## 32:9 follow-up

Add `R32_9` to `Settings::AspectRatio` (layout, `CalculateWidth`, Qt and Android labels), add a second
`BUILTIN_PATCHES` row with the same name, `.aspect_ratio = R32_9` and records from
`OcarinaUltrawide::Make(32.0 / 9.0)`, and list and flip each built-in name once.
