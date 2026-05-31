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

## On security

Don't trade freedom for so-called security.

It makes a convenient excuse for keeping control over how a product is used, but forcing people to
export their data and to expose more and more hardware to the internet has never been about security.

A machine fully disconnected from the internet will always be more secure than one that isn't.

How are the most sensitive projects handled? On an internet-facing cloud? No. On a local network,
completely cut off from the rest. A smaller attack surface means lower risk.

You don't need to lock your door if you live on the moon.
