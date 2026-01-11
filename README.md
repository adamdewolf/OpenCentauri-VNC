# OpenCentauri-VNC

**Remote Framebuffer VNC Server for the Elegoo Centauri Carbon**

---

## Overview

`OpenCentauri-VNC` is a lightweight VNC (RFB) **server** designed specifically for the **Elegoo Centauri Carbon (ECC)** running **OpenCentauri**.

It streams the printer’s touchscreen interface directly from `/dev/fb0`, allowing you to remotely view **exactly what appears on the printer’s display** without modifying firmware, rebuilding the UI, or installing heavyweight services that could impact print stability.

---

## Important Clarifications

- This project implements a **VNC server**, not a VNC viewer/client.
- The current implementation is **view-only** due to platform constraints.
- Input injection (touch, keyboard, mouse) is **not implemented at this time**, but interactive access **may be possible in the future** if safe and reliable mechanisms become available.
- Installation via USB (`/mnt/exUDISK`) is **temporary** and intended **only for development and testing**. No permanent changes are made to the printer firmware by default.

---

## Why This Exists (Centauri-Specific)

The Elegoo Centauri Carbon:

- Uses a **direct framebuffer UI** (`/dev/fb0`) rather than X11 or Wayland
- Runs on an embedded Linux platform (**OpenCentauri**)
- Does not provide native remote display functionality
- Should not be burdened with background services that could impact long prints

`OpenCentauri-VNC` provides a **low-impact, display-only** solution that respects these constraints.

---

## Key Features

- Reads directly from `/dev/fb0` used by the Centauri Carbon UI
- Requires **no modification** of firmware or UI components
- Built as a **static binary** (musl) to avoid glibc compatibility issues
- Predictable and bounded resource usage
- Adjustable frame rate (default: **3 FPS**)
- Single-client connection
- Uses standard **RFB / VNC 3.8**
- RAW encoding only

---

## Current Limitations

- No input injection (touch, keyboard, or mouse)
- No authentication or encryption
- No compression or advanced encodings
- Not intended as a general-purpose desktop VNC server

> These are **current technical limitations**, not intentional product features.  
> Future versions may explore interactive access if it can be implemented safely without risking print reliability.

---

## Primary Use Case

With `OpenCentauri-VNC`, you can:

- Monitor prints remotely
- Observe printer state and progress
- Verify temperatures, menus, and UI behavior
- Capture screenshots or recordings for diagnostics
- Check printer status without physical access

All without rebuilding firmware or modifying vendor software.

---

## Secondary Applications

Although designed specifically for the Elegoo Centauri Carbon and OpenCentauri, this approach may also be applicable to other embedded Linux devices that render their UI directly to `/dev/fb0`, such as:

- Other embedded printers or CNC controllers
- LVGL-based appliances
- OpenWrt systems with framebuffer UIs
- Industrial HMIs or kiosk devices

> These are incidental use cases, not the primary focus of this project.

---

## Build Instructions

### Requirements

- **Zig 0.12.0 or newer**
- Linux build environment (Ubuntu / WSL tested)

### Build (Static ARM Binary)

```bash
zig cc -O2 -static \
  -target arm-linux-musleabihf \
  -mcpu=generic+v7a \
  -o OpenCentauri-VNC fb0rfb.c
```

This produces a **fully static ARM binary** compatible with OpenCentauri.

---

## Installation (Development / Temporary)

> ⚠️ **Note:** Installation via USB (`/mnt/exUDISK`) is temporary and intended only for development and testing.

Copy the binary to the printer:

```bash
scp OpenCentauri-VNC root@PRINTER_IP:/mnt/exUDISK/bin/
```

On the printer:

```sh
cd /mnt/exUDISK/bin
chmod +x OpenCentauri-VNC
nice -n 19 ./OpenCentauri-VNC --fps 3
```

Connect using a VNC viewer to:

```text
PRINTER_IP:5900
```

### Recommended VNC Viewers

- **TigerVNC**
- **TightVNC**
- **UltraVNC**

---

## Command-Line Options

```text
-f /dev/fb0     Framebuffer device (default: /dev/fb0)
-p 5900         TCP port (default: 5900)
--fps N         Frames per second (default: 3, max: 15)
```

> Lower FPS results in lower CPU usage.

---

## Technical Summary

- **Role:** VNC **server**
- **Framebuffer source:** `/dev/fb0`
- **Protocol:** RFB / VNC 3.8
- **Encoding:** RAW
- **Binary:** Static (musl)
- **Security:** None (LAN use only)
- Designed for predictable, low-impact operation

---

## Security Notice

This software provides **no authentication or encryption**.  
It is intended for **trusted local networks only**.

> **Do not expose it directly to the internet.**

---

## Development Acknowledgement

This project was developed with assistance from **ChatGPT 5.2**.  
Design discussion, code review, documentation, and build strategy were aided by AI-generated analysis and suggestions, with final implementation and testing performed by the project author.

---

## License

**MIT License**
