# Changelog

All notable changes of BambuStudio-LAN-RESTORED. Versions follow upstream BambuStudio numbering;
the last field is bumped for fork releases (the first three fields must match the Bambu network
plug-in), and tags carry a `-lan` suffix.

## v02.07.00.56-lan

### Added
- **MCP server for AI agents.** Bambu Studio can expose a [Model Context Protocol](https://modelcontextprotocol.io)
  server (Streamable HTTP, `http://127.0.0.1:13619/mcp`, loopback only) so that agents such as Claude drive the
  running application. Opt-in from *Preferences → Allow AI agents to control Bambu Studio (MCP server)*, port
  configurable, optional bearer token (`mcp_server_token` in `BambuStudio.conf`).
- 44 tools covering:
  - projects: open, new, save, import models, undo/redo, plate screenshots;
  - plates: list, add, delete, select, rename;
  - objects: list, move, rotate, scale, mirror, move to plate, duplicate, delete, rename, arrange, auto-orient;
  - settings: search, read and change slicer settings globally or per object/part, list and select presets;
  - filaments: list, change preset/colour, add, remove, assign to objects or parts;
  - painting: seams, supports, fuzzy skin and colours, with brush strokes, spheres, height bands, boxes or
    facet orientation;
  - slicing: slice a plate or all plates, read estimated time and filament usage, export `.gcode` / `.gcode.3mf`;
  - printers: list, live status (job, temperatures, fans, AMS, HMS), select, pause/resume/stop, chamber light.
- Tools never display dialogs: user choices are explicit tool arguments (`discard_changes`, `unsaved_changes`,
  `run_post_process_scripts`), and any other prompt raised internally is answered automatically and reported to
  the agent.

## v02.07.00.55-lan

Built on upstream BambuStudio `02.07.00.55`.

### Added
- Browse SD-card storage over FTPS in LAN Only Mode (`Device > Storage`), without the proprietary cloud tunnel.
- Print from the SD card (Models card "print" button) through a `project_file` command published over local MQTT.
- Download, delete and thumbnails for SD-card files over FTPS, with a local cache.

### Fixed
- `ERROR_PIPE` "Reconnecting the printer…" popup and HMS `05004002` rejection when printing from the SD card in
  LAN Only Mode.
- macOS: Cmd+H hide-app shortcut.
- Worker thread stack raised to 16 MB to survive CGAL emboss.
