# Controller model and artwork

The shell comes from Valve Corporation's **2026 Steam Controller** CAD,
`sc_solid_stl_20260429.stl`, published in the
[Steam Controller repository](https://gitlab.steamos.cloud/SteamHardware/SteamController).
Copyright 2026 Valve Corporation. The source is licensed CC BY-NC-SA 4.0.
See [license.txt](license.txt).

`controller.stl` is the reduced rendering mesh in metres, preserving the original
CAD axes. `controller.json` records source hashes and bounds. The surface file
adds UVs, shading normals and material maps without moving vertices.
`controller.buttons` describes the animated controls and their pivots and travel.

Texture donor: **Steam Controller 2025 by Ranguel**, from
[Sketchfab](https://sketchfab.com/3d-models/steam-controller-2025-fe10b1da83e54dae953f15d898ad6197),
licensed [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
Markings, grain and rubber roughness were mapped to Valve's CAD.
Colors and roughness were assigned in code. Shading normals were adjusted around
the shell crease and thumbstick junctions. No generative image model was used.
The Valve CAD terms above continue to apply.

The app icon is `Steam Controller/Vector/controller_steam_new.svg` from
[Kenney's Input Prompts 1.5A](https://kenney.nl/assets/input-prompts), licensed CC0.
The SVG is unchanged. The PNG is a proportional rasterization of that SVG.
See [controller-icon-license.txt](controller-icon-license.txt).

Button glyphs are loaded from the local Steam installation. Fonts use SteamVR's
installed typeface when available, or a system fallback. They are not bundled here.

Per-controller calibration is stored privately in `app-data/calibrations`.
It is excluded from source assets and release archives.
