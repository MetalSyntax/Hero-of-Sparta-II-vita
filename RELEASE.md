# Hero of Sparta II (PS Vita) — Release v1.0

First public release of the native iOS **Mach-O loader** port of *Hero of Sparta II* (Gameloft) for the PlayStation Vita. The game is playable from the intro trailer through the menus and levels, with correct 3D graphics, music and sound, fullscreen 16:9 output, hidden on-screen controls and a full physical button mapping.

---

## 📦 Release Package Contents

1. **`heroofspartaii.vpk`** (v01.00): PlayStation Vita application package with the Mach-O loader, vitaGL renderer, hardware video decoder and physical input handling.
2. **`rebase.bin`**: binary relocation table (**39,357** absolute 32-bit pointers) for the iOS ARM executable `HOS2` of *Hero of Sparta II* 1.0.2.

---

## 🚀 Highlights

### 🎨 Graphics
- GLES 1.x over **vitaGL** (`SOFTFP_ABI=1`), 320×480 iOS framebuffer rotated onto the 960×544 screen.
- **Fullscreen 16:9 without black bars**: the 480×320 image is stretched to 960×544; HUD, menus and cutscenes reach the screen edges and touch input follows the same mapping.
- Fixed black wedges and broken geometry: the game draws from memory it has already freed (harmless on iOS); freed blocks used by a draw are kept alive.
- ARM switch jump tables in `__TEXT.__text` are relocated, fixing random crashes and wrong code paths.

### 🎮 Controls
- **On-screen touch controls hidden (0% opacity) by default**; **L + R** toggles them to 100%.
- Physical buttons press the game's own on-screen buttons (positions editable in `controls.txt`).

### 🎬 Video & Audio
- `HoS_trailer.m4v` plays with hardware H.264 (`SceVideodec`) and AAC audio (FFmpeg); skippable with Cross, Circle or Start.
- Music and sound effects through the CoreAudio / OpenAL layer.

### 💾 Saves
- iOS `Documents/` is redirected to `ux0:data/heroofspartaii/saves/`; Continue resumes from the last checkpoint.
- Every save file read, write and delete is logged with a `[SAVE]` tag so progress problems can be traced.

---

## 🎮 Complete Controls Reference

| PS Vita Input | Action |
| :--- | :--- |
| **Front Touchscreen** | Full touch UI (menus and on-screen buttons) |
| **Left Stick / D-Pad** | Move (virtual joystick) |
| **Cross (✖)** | Attack |
| **Right Stick** | Directional attack (swipe on the attack button) |
| **Square (◼)** | Jump |
| **Triangle (▲)** | Defend |
| **Circle (●)** | Grab enemy / start finisher cutscene |
| **Start** | Shield button |
| **Select** | Pause menu |
| **L + R** | Show / hide on-screen touch controls (0% / 100%) |
| **Cross / Circle / Start** | Skip the trailer |

---

## 🚀 Player Installation Guide

1. **PS Vita requirements**:
   - Custom Firmware (**Enso 3.60 / 3.65** recommended).
   - `kubridge.skprx` under `*KERNEL` in `ur0:tai/config.txt`.
   - `libshacccg.suprx` in `ur0:data/` (obtainable via **ShaRKBR33D**).

2. **Game assets**:
   - Extract `Payload/HOS2.app/` from your decrypted iOS 1.0.2 `.ipa` (FairPlay `cryptid 0`).
   - Copy its contents to `ux0:data/heroofspartaii/game_app/`.
   - Copy `rebase.bin` to `ux0:data/heroofspartaii/rebase.bin`.
   - Layout:
     ```text
     ux0:data/heroofspartaii/
     ├── game_app/      (contents of HOS2.app: HOS2, *.pak, gui, models, ...)
     ├── rebase.bin
     ├── controls.txt   (created on first launch)
     ├── saves/         (created on first launch)
     └── logs/          (created on first launch, one log per run)
     ```

3. **Install the VPK**: transfer and install `heroofspartaii.vpk` with VitaShell.

---

## ⚠️ Known Issues

- Slow start-up (~400 sound files are opened at boot); the game's launch image is shown meanwhile.
- Gameplay runs at roughly 20-30 fps (CPU-bound game logic); menus run at 60 fps.
- Online features (Gameloft Live, ads) are disabled by design.

---

## 📋 Changelog

### v1.0.0 (2026-11-04)
- Fullscreen 16:9 output (stretched image, touch input remapped to match).
- On-screen touch controls hidden by default, toggled with L + R.
- Button mapping: Cross attack, Square jump, L + R reserved for the touch-control toggle, Circle grab / finisher, Triangle defend, Start shield, Select pause.
- Right stick performs directional attacks.
- The game's own launch image (Gameloft logo, rotated to landscape) is shown while the game loads instead of a blank screen.
- Removed the L + R + Triangle trailer debug shortcut.
- `[SAVE]` logging of every save file operation.
- Fixed black wedges / broken geometry (freed draw arrays kept alive, with a separate larger quarantine for mesh arrays so they survive new-stage loads) and relocated ARM switch jump tables.
- Removed graphics and heap diagnostics from the hot path, and enabled triple buffering (gameplay ~19-21 -> ~22-29 fps).
