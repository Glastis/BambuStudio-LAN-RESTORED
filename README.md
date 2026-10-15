# BambuStudio-LAN-RESTORED

> **To Bambu Lab:** thanks for the 10/10 hardware. We'll take it from here. You're no longer involved.

A fork of [Bambu Lab's **BambuStudio**](https://github.com/bambulab/BambuStudio) that brings back
the **Storage** browser and **print-from-SD** when the printer runs in **LAN Only Mode**.

The official build disables SD-card access as soon as the printer is in LAN Only Mode. The Storage
tab only shows *"Browsing file in storage is not supported in LAN Only Mode."*:

<p align="center">
  <img src="doc/official.png" alt="Official BambuStudio: Storage browsing blocked in LAN Only Mode" width="820">
</p>

This fork talks to the printer's built-in **FTPS** server and local **MQTT** directly, so you can
list, preview, download, delete and **print** the files on the SD card, no cloud or internet required:

<p align="center">
  <img src="doc/conveniented.png" alt="LAN-RESTORED: SD-card models browsable and printable in LAN Only Mode" width="820">
</p>

## What's different from upstream

- **Browse / download / delete** Timelapse and Model files over FTPS in LAN Only Mode, with real
  thumbnails and a local cache.
- **Print from SD** over local MQTT: the `project_file` command is published directly with an
  `ftp://` URL the printer resolves itself, instead of the cloud-only path that fails in LAN Only Mode.
- **MCP server for AI agents**: Claude (or any MCP client) can drive the running Bambu Studio, see below.

## AI agents (MCP)

Bambu Studio can expose a [Model Context Protocol](https://modelcontextprotocol.io) server so that
agents act on the live GUI exactly like a user would (every change shows up on screen and can be undone).

Enable it in **Preferences → Allow AI agents to control Bambu Studio (MCP server)**. It listens on
`http://127.0.0.1:13619/mcp` (loopback only, port configurable next to the checkbox). Then, for Claude Code:

```sh
claude mcp add --transport http bambustudio http://127.0.0.1:13619/mcp
```

Tools never display dialogs: the choices Bambu Studio would ask the user are explicit tool arguments
(`discard_changes`, `unsaved_changes`, `run_post_process_scripts`...). Any other prompt raised internally is
answered automatically (OK for information, Cancel/No for questions) and its text is returned to the agent.

Any client speaking the Streamable HTTP transport works the same way. To require a token, set
`"mcp_server_token": "<secret>"` in `BambuStudio.conf` and send `Authorization: Bearer <secret>`
(`claude mcp add ... --header "Authorization: Bearer <secret>"`).

| Area | Tools |
| --- | --- |
| Project | `get_app_state`, `new_project`, `open_project`, `save_project`, `import_models`, `undo`, `redo`, `screenshot` |
| Plates | `list_plates`, `add_plate`, `delete_plate`, `select_plate`, `rename_plate` |
| Objects | `list_objects`, `get_object`, `select_objects`, `transform_object` (move/rotate/scale/mirror), `move_to_plate`, `rename_object`, `duplicate_object`, `delete_objects`, `arrange`, `auto_orient` |
| Settings | `search_settings`, `get_settings`, `set_settings` (global or per object/part, e.g. supports), `list_presets`, `select_preset` |
| Filaments | `list_filaments`, `set_filament`, `add_filament`, `remove_filament`, `assign_filament` |
| Painting | `paint_facets` (seams, supports, fuzzy skin, colours, by brush stroke / sphere / box / facing direction), `clear_painting`, `paint_info` |
| Slicing | `slice`, `get_slice_result`, `export_gcode`, `export_sliced_3mf` |
| Printers | `list_printers`, `get_printer_status` (job, temperatures, AMS, HMS), `select_printer`, `printer_control` (pause/resume/stop/light) |

## On security

Don't trade freedom for so-called security.

It makes a convenient excuse for keeping control over how a product is used, but forcing people to
export their data and to expose more and more hardware to the internet has never been about security.

A machine fully disconnected from the internet will always be more secure than one that isn't.

How are the most sensitive projects handled? On an internet-facing cloud? No. On a local network,
completely cut off from the rest. A smaller attack surface means lower risk.

You don't need to lock your door if you live on the moon.
