# Port Tool

A lightweight, zero-dependency, **strictly read-only** Windows native desktop application to monitor hardware ports on any Windows laptop or PC in real time.

## Features

- **All Ports on One Main Page (`Ports` Tab)**: Displays every detected hardware port on the machine in a single live-updating view (sampled every **0.5s**) with current-instance telemetry:
  - **USB Ports** (Root Hubs & External Hubs — connected devices, negotiated link speed, VBUS power budget, and live USB mass storage throughput)
  - **Network Ports** (Ethernet, Wi-Fi, WWAN — live To/Out TX and Fro/In RX transfer speeds via `GetIfTable2`)
  - **Power / Charging Ports** (AC / USB-C PD / Battery — live Watts Accepted while charging or Delivered while discharging via `IOCTL_BATTERY_QUERY_STATUS` + ACPI/RAPL Energy Meters)
  - **Display / Video Ports** (HDMI, DisplayPort, eDP, DVI, VGA — active resolution, refresh rate, and video output stream rate)
  - **Audio Ports** (Speakers, Headphones, Microphones — active stream playback/capture bitrate)
  - **Serial / COM Ports** (RS-232 / Virtual COM ports)
- **System Hardware Performance (`System Hardware` Tab)**: Separate overview tab covering **Processor (CPU)**, **GPU(s)**, **Physical Disk(s)**, and **Memory (RAM)** with live **Utilization**, **Data/Usage**, **Power**, and **Temperature (°C)** on the main page. Clicking any component opens Task-Manager-level specs and live 0.5s graphs (Utilization, Data/Memory, Power, and Temperature).
- **On-Demand Live Graphs**: Graphs are **only shown when you click a specific port name** on the Main Page:
  - **Data Transfer Speed vs Time** (To / Out & Fro / In, with interactive hover crosshair & tooltip)
  - **Power Delivered / Accepted vs Time** (color-coded Delivered vs Accepted, with interactive hover crosshair & tooltip)
- **Honest `N/A` Handling**: Unused ports or metrics not exposed by hardware clearly display `N/A`.
- **Interactive UI with Dark & Light Mode**: Hardware-accelerated Direct2D + DirectWrite UI with instant Dark/Light mode toggle, DWM title-bar theme sync, category filters, and DPI awareness.
- **Zero External Dependencies & Size-Optimized**: Compiled with MSVC (`/O1 /Os /GL /MT /LTCG`) into a single standalone `PortTool.exe` (~200–350 KB) that runs on any Windows 10/11 PC without installing runtimes or frameworks.
- **Strictly Read-Only Guarantee**:
  - Runs with `asInvoker` (no administrator elevation required).
  - Uses exclusively read-only Windows APIs (`GENERIC_READ` / `0` access, `KEY_READ`, `STGM_READ`, `GetIfTable2`, `QueryDisplayConfig`).
  - Never modifies device properties, registry settings, power states, or system configuration.

## Building from Source

Requires **Visual Studio Build Tools** (MSVC C++20) and the **Windows 10/11 SDK**.

```bat
build.bat
```

Output: `build\PortTool.exe`

## License

MIT

