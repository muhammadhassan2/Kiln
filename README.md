# Shader Studio (Shader_Simulation)

A live shader editor in the style of KodeLife, built for mobile video effect and transition development. It runs on desktop OpenGL 4.1 core. The rendering code sits behind a small interface (`src/rhi`), so a Metal renderer can be added later as a new folder.

## Build

Open the folder in CLion and run it. To build from a terminal instead:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/Shader_Simulation [project.ssproj | shader.frag | media files...] [--template N] [--preset android|iphone|desktop]
```

CMake downloads these dependencies the first time you configure, each pinned to a fixed version (the first configure takes a few minutes):

- GLFW 3.4
- Dear ImGui 1.92.9b (docking branch)
- goossens/ImGuiColorTextEdit (code editor and diff view)
- stb_image / stb_image_write
- nativefiledialog-extended (native file dialogs)
- nlohmann/json
- glslang 16.6
- SPIRV-Cross
- GLEW (Windows and Linux only)

**Runtime tools** (optional; the features below are disabled without them):

| Tool | Used for | macOS install |
|---|---|---|
| `ffmpeg` / `ffprobe` / `ffplay` | Video, camera and audio textures; MP4/ProRes export; hearing audio while you preview | `brew install ffmpeg` |
| `curl` | AI assistant | already installed |
| Xcode (`xcrun metal`) | Checking that the converted Metal code compiles | Xcode |

## Features

| Area | What you get |
|---|---|
| **Projects** | `.ssproj` (JSON) files store passes, textures, uniform values, keyframes, settings and version history. Use Save / Save As / Open Recent. Your session is restored automatically when you start the app again. |
| **Templates** | File → New from template: Basic, Video effect (GLSL ES), Transition, Shadertoy, Multipass feedback, Audio reactive. |
| **Multipass** | Up to 4 buffer passes (Buffer A–D, 16-bit float) plus the Image pass. Read another pass with `u_bufferA`…`u_bufferD`. Read this pass's previous frame with `u_prev`. `iChannel0..3` can be pointed at any texture or buffer (iChannels… button). |
| **GLSL dialects** | Modern `#version 330+` shaders run as written. Older GLSL ES 1.00 shaders (`varying`, `texture2D`, `gl_FragColor`), `#version 300 es`, Shadertoy `mainImage()` and gl-transitions `transition()` are converted automatically; error line numbers still match your file. `#include "file.glsl"` looks in the shader's folder, then the project folder, then `<config>/includes`. |
| **Textures** | Images, video files, camera, audio file, microphone, noise, checker. There is no fixed count; the GPU's limit applies. Slot *i* is `u_tex<i>` or `iChannel<i>`. Other sampler names take slots in the order they are declared. |
| **Audio** | Audio textures are 512×2: row 0 is the spectrum, row 1 the waveform (same layout as Shadertoy). Built-in level uniforms: `u_audioLevel`, `u_bass`, `u_mid`, `u_high`. Tick "Hear it" to play the file while previewing. |
| **Video** | Video textures follow the timeline. Exports decode the video frame by frame, so every export frame uses exactly the right video frame. |
| **Uniforms** | Found automatically when the shader compiles. Comments next to a uniform become controls: `@slider`, `@color`, `@toggle`, `@drag`, plus the gl-transitions `// = default` comments. Each uniform can be Manual, Time, Loop 0..1, Keyframes, or Video position (follows the first video's playback). |
| **Defines** | Every `#ifndef NAME` + `#define NAME value` pair (e.g. `EFFECT_ID` in an uber-shader) appears at the top of the Uniforms panel. Changing it there recompiles at once (no typing delay; the mouse wheel steps numbers), and variants you already compiled are reused from a cache, so switching back is instant. Editing the `#define` line in the code takes over again. Saved with the project. |
| **Compile speed** | Shader → typing delay sets the pause after your last keystroke before auto-compiling (default 120 ms; 0 = every keystroke). Debug builds still compile ImGui, the editor, glslang and SPIRV-Cross with `-O2` (`SHADER_STUDIO_FAST_DEPS`, on by default) so big shaders stay responsive. |
| **Timeline** | Duration, loop and a scrub bar. Keyframe tracks show diamonds you can drag. Right-click a key to change its easing (Linear, Ease in, Ease out, Ease in-out, Step). Double-click a track to add a key. |
| **Transition** | Choose the From and To textures (`u_from` / `u_to`, or `getFromColor` / `getToColor`), then scrub, play or keyframe `progress`. |
| **Device presets** | Desktop / Android phone / iPhone. The phone presets set 1080×1920, top-left UV origin and mobile checking. |
| **Mobile / Metal** | Mobile check: on every compile, glslang checks the original source as a phone would (GLSL ES), e.g. missing `precision`. Metal: GLSL → SPIR-V → MSL for iOS or macOS, with a button to check the result with Xcode's `metal` compiler. |
| **Export** | PNG frame, PNG sequence, MP4 (H.264) or MOV (ProRes 422 HQ), at any size, fps and length. Audio from the first audio texture can be included. |
| **Preview** | View any pass, isolate a channel (R / G / B / A / luma), and use Inspect: a loupe that shows the exact pixel values. |
| **Performance** | GPU time per pass (GL timer queries), CPU frame-time graph, and cost per megapixel. |
| **History** | A snapshot is saved for every successful compile. Compare any snapshot with the current code side by side, and restore it (Undo also works). |
| **Library** | Saves projects with thumbnails to `~/Documents/ShaderStudio/Library`. Search them and click one to open it. |
| **Autocomplete** | GLSL keywords and built-ins, plus your uniforms and identifiers (pops up while typing; Ctrl+Space forces it). |
| **Output window** | View → Output window shows the output windowed or full screen on any monitor (for projectors or VJ use). |
| **AI assistant** | Uses the Claude API through `curl`: write a new shader, change the current pass, or explain and fix errors. Apply the result with one click. The key comes from `ANTHROPIC_API_KEY` or the panel; the model list is loaded from the API. |
| **Overlay mode** | F2 shows the code over the full-screen output; switch passes from the top bar. Esc exits. |

### Built-in uniforms

`u_time`, `u_timeDelta`, `u_frame`, `u_resolution`, `u_mouse`, `u_texCount`, `u_audioLevel`, `u_bass`, `u_mid`, `u_high`, and `ratio` (for transitions). The Shadertoy names also work: `iTime`, `iTimeDelta`, `iFrame`, `iResolution`, `iMouse`, `iDate`.

### Shortcuts

| Key | Action |
|---|---|
| F5 / Shift+F5 | Compile the current pass / all passes |
| Cmd+S / Cmd+Shift+S | Save project / Save As |
| Cmd+O | Open project |
| Cmd+E | Export |
| F2, Esc | Overlay mode on / off |
| Ctrl+Space | Autocomplete |

## Layout

```
main.cpp                 window, GL context, ImGui, main loop
src/rhi/                 render interface + OpenGL backend (clear, readback, timer queries)
src/shader/              dialect conversion & #include, compile-log parsing, @annotations, glslang/SPIRV-Cross
src/media/               child processes (ffmpeg/curl/xcrun), video/camera decoding, audio FFT
src/textures/            texture slots (images, video, camera, audio, procedural)
src/ai/                  Claude API client
src/app/                 App: passes, compiling, rendering (App.cpp), files & projects (AppFiles), UI (AppUI),
                         preview (AppPreview), textures panel (AppTextures), timeline/transition/presets
                         (AppTimeline), export/performance/history/Metal/library/AI (AppTools), templates,
                         second output window
```

Settings, the window layout and the autosaved session are stored in `~/Library/Application Support/ShaderStudio` (macOS), `%APPDATA%/ShaderStudio` (Windows) or `~/.config/ShaderStudio` (Linux).

## Testing without a screen (CI)

- `SHADER_STUDIO_FRAMES=N` quits after N frames.
- `SHADER_STUDIO_EXPORT=/path/out.mp4` (or `.mov`, `.png`, or a folder for a PNG sequence) renders 2 s at 540×960, then continues running.
