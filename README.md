# VR Cube

A tiny mixed-reality toy for the **Steam Frame** (SteamOS, ARM64). A cube floats in front of you over passthrough and SteamVR's desktop mode. Only the cube is drawn; everything around it stays see-through, and your desktop stays usable.

It's also a working example of a SteamVR **overlay** that:

- renders a correct **3D object** on a flat overlay, using head-tracked, per-eye off-axis ("window") projection,
- runs **alongside desktop mode** without replacing it,
- gets **controller input while the SteamVR dashboard owns the controllers**, through laser mouse and scroll events.

## Controls

Point the SteamVR laser at the cube:

| Input | Action |
|---|---|
| Quick click (trigger or bumper) | Change color (8-color cycle) |
| Hold the click (≥ 0.3 s) | Grab: the cube glides onto your laser and follows your hand |
| Stick up / down | Push the cube away / pull it closer along the laser |
| Stick left / right while grabbing | Past about half-way, takes over from the auto-spin and turns the cube (it stays level) |
| Let go | Drop it; the auto-spin resumes |

When the cube first appears, it's 80 cm in front of where you're looking, slightly below eye level. It always faces the same way in the room, so you can walk around it.

## Building

On the Steam Frame itself (SteamOS has the toolchain and headers):

```sh
make            # builds both programs
./vr-cube-overlay
```

Requirements:

- SteamVR installed at `/opt/steamvr` (the Steam Frame default). The Makefile links against `libopenvr_api.so` and `libopenxr_loader.so` from `/opt/steamvr/bin/linuxarm64`.
- Mesa EGL and OpenGL 3.3, plus the OpenXR headers (for the optional `vr-cube` scene app).
- A C/C++ compiler and `make`.

`actions.json` and the `bindings_*.json` files must sit next to the binary.

## Files

| File | What it is |
|---|---|
| `overlay.cpp` | **The main program**: the SteamVR overlay version described above. |
| `main.c` | An earlier version as a full OpenXR scene app. It takes over the headset, so no passthrough and no desktop. It's kept as a minimal OpenXR + EGL example. |
| `actions.json`, `bindings_*.json` | SteamVR Input manifest. It's only used outside desktop mode (L1/R1 color, L2/R2 grab, stick push/pull). |
| `third_party/openvr/` | `openvr.h` from [ValveSoftware/openvr](https://github.com/ValveSoftware/openvr) v2.15.6, BSD-3-Clause. |

## How it works

### Overlay instead of scene app

An OpenXR or OpenVR *scene* app replaces whatever the headset is showing, including desktop mode and passthrough. A SteamVR overlay (`VRApplication_Overlay`) is composited on top instead. The overlay texture is transparent except for the cube.

### 3D on a flat overlay

The overlay uses `VROverlayFlags_SideBySide_Parallel`, so the left half of the texture goes to the left eye and the right half to the right eye. Each frame, the app:

1. predicts the headset pose for when the frame will reach the display,
2. turns the overlay (yaw only) to face you,
3. converts each eye's position (`GetEyeToHeadTransform`) into overlay space,
4. renders the cube through the overlay rectangle with an off-axis frustum from that exact eye position.

The overlay behaves like a window into a small 3D space, so parallax stays right as you move. Rendering is synced with `WaitFrameSync` and submitted as double-buffered OpenGL textures. `SetOverlayRaw` uploads flicker, so they're only a fallback.

### Input under desktop mode

Steam Frame desktop mode is the SteamVR dashboard. While it's visible, SteamVR turns off normal controller input for every app (`IsInputAvailable() == false`), and that includes SteamVR Input actions, even with the experimental global action set priority. What an overlay does receive:

| What you do | What the overlay receives |
|---|---|
| Any click (R1, R2) | `VREvent_MouseButtonDown` / `Up` (always button 1) |
| Stick | `VREvent_ScrollSmooth` with `VROverlayFlags_SendVRSmoothScrollEvents` (≈ ±0.22 per event at full deflection) |

Discrete scroll events are left off on purpose: their "notches" make SteamVR vibrate the controller on every tick.

A few details that keep this reliable:

- **Hold detection** uses each event's `eventAgeSeconds`, not frame counts. When desktop mode is busy (its keyboard is open, for example), the press and release can arrive together.
- **Keyboard:** desktop mode moves its on-screen keyboard to whatever overlay the laser focuses. The app closes any keyboard opened for the cube (`HideKeyboard`); yours comes back when you return to your window.
- **Click hit test:** clicks only count on the cube, not the transparent area around it. The app reads back the alpha under the laser in both eye images.

## Debugging

Environment variables:

| Variable | Effect |
|---|---|
| `VRCUBE_DEBUG=1` | Log input, focus and keyboard events to stdout |
| `VRCUBE_DUMP=frame.pam` | Save the first rendered frame (RGBA, PAM format) |
| `VRCUBE_RAW=1` | Use `SetOverlayRaw` uploads instead of GL textures |

SteamVR's own logs are in `~/.local/share/Steam/logs/` (`vrserver.txt`, `vrclient_vr-cube-overlay.txt`).

## Known limitations

- **Laser only:** in desktop mode, every button arrives as the same laser click, so R1 and R2 can't do different things.
- **Viewing angle:** the 3D effect is a window: from far off to the side or behind, the panel's edges clip the cube.
- **Platform:** it has only been tested on a Steam Frame (SteamVR 2.18, Adreno 750, Mesa Zink).

## Roadmap

The goal is to go beyond the cube: **load any 3D object** and place it in your space the same way, over passthrough and desktop mode.

- Load models from files: **STL** first, then other common formats such as OBJ, glTF/GLB and PLY.
- Pick the file to open from the command line or a desktop file picker.
- Scale the model automatically so it fits in front of you, then resize it with the controls.
- Shading that suits imported meshes (normals, and colors or textures when the file has them).

## License

MIT, see [LICENSE](LICENSE). `third_party/openvr/openvr.h` is BSD-3-Clause (Valve Corporation), see [third_party/openvr/LICENSE](third_party/openvr/LICENSE).
