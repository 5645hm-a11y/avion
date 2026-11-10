# AVION samples: your interface, AVION inside

Each sample is a complete player whose controls are its own, with AVION playing inside its window through `avion.dll` (SDK API 12, [embedding](../docs/sdk.md)). The pattern is the same in every language:

1. On a thread of its own, create the engine with `config.parent_window` set to your video panel's window handle (and `MEDIA_MODULE_HUD` left out, so the engine draws no control), then `open` and `run`.
2. From your UI thread, send what the viewer does with `media_engine_post_command`: `"pause"`, `"play"`, `"volume 40"`, `"go to 1:23"`, `"forward 10 seconds"`, `"mute"`, or anything else in the [grammar](../docs/intents.md), in nine languages.
3. About 60 times a second, take what happened with `media_engine_poll_event` (opened, state, position, seek landed, command answered, fullscreen asked, ended), and read the clock with `media_engine_state`.

| Sample | | Run |
|---|---|---|
| `csharp/AvionPlayer` | .NET 8 WinForms: drawn seek bar and volume, Segoe Fluent icons, drag and drop, fullscreen on request | `dotnet run -c Release -- video.mp4` |
| `python/avion_player.py` | tkinter and ctypes, about 150 lines | `python avion_player.py video.mp4` |
| `quickstart.c` | C99: a live camera in; perception, a tensor and event spikes out | `bin\media_quickstart.exe` |
| `avion_showcase.c` | C99: the program that made the site's showcase pictures | `bin\media_showcase.exe` |

`avion.dll` is looked for in the build (`build/Release`) or in the bundle's `bin`. It needs Windows 10 or 11 x64 and a Direct3D 12 GPU; it depends on Windows' own libraries only.

A game engine, a compositor or a 3D scene can take the pictures as textures instead of a window (SDK API 13, [texture output](../docs/sdk.md)): set `config.output_flags = MEDIA_OUTPUT_TEXTURE` with a size, open the three shared textures and two shared fences on your own Direct3D 11 or 12 device, and each frame acquire the newest, wait for its fence on your GPU, sample it, signal your fence and release it.
