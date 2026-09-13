# Cardputer Adv — Dual-Screen Game Station (GB/GBC)

Fork of [eghor2005/Cardputer-Game-Boy-Color](https://github.com/eghor2005/Cardputer-Game-Boy-Color) (Game Station) aimed at **M5Stack Cardputer Adv** with an **external ILI9341**, **max SPIFFS** partitioning, and optional **GameShark-style cheats**.

## Credits

- **Idea, hardware goals, and direction:** [Dirt](https://github.com/moes-bobandy) (`moes-bobandy`) — dual-screen Cardputer Adv setup, large-SPIFFS ROM storage, GameShark cheat workflow, and iterative playtesting.
- **Upstream Game Station / gnuboy Cardputer port:** [eghor2005/Cardputer-Game-Boy-Color](https://github.com/eghor2005/Cardputer-Game-Boy-Color).
- Implementation assistance via GBAbot / Cursor tooling.

## Why this fork

With the **internal ST7789 left as a simple status stub** (“Dual Screen”) and gameplay driven on the **external ILI9341**, plus a **large SPIFFS** layout (`partitions_5m_spiffs_8mb`), this fork is validated playing **GB/GBC ROMs up to about 5 MB**.

In Dirt’s testing, the earlier “big SPIFFS” / internal-screen-oriented builds still struggled past roughly **~2 MB** ROMs; this dual-screen Adv build is what made the larger titles practical.

### Highlights

- **Dual-screen:** external ILI9341 gameplay (RGB565 / COLMOD locked for shift-free output); internal panel used lightly.
- **Large SPIFFS:** `partitions_5m_spiffs_8mb` — playtested with ROMs up to ~5 MB (vs ~2 MB ceiling on prior big-SPIFFS / internal-screen setups).
- **GameShark cheats:** in-game menu (`c`), SD cheats at `/gbc_cheats/<ROM basename>.cht`, up to 512 codes, apply each frame; see [`CHEATS.md`](CHEATS.md).
- **Save stability:** SPI-safe SRAM load/save on shared bus (display idle → quiesce → SD → resume).

## Hardware notes (typical PorkChop-style external panel)

- ILI9341 on SPI3 (shared with SD): SCK 40, MOSI 14, CS 5, DC 6, RST 3; SD CS 12.
- Target: Cardputer Adv / StampS3, PlatformIO env `m5stack-stamps3`.

## Build

```bash
pio run -e m5stack-stamps3
```

Flash the resulting firmware / launcher-ready `.bin` via your usual Cardputer Launcher / USB flow. Prefer the **max SPIFFS** partition CSV shipped with this tree (`partitions_5m_spiffs_8mb.csv`).

## Cheats (short)

1. Put `YourRom.cht` next to basename of `YourRom.gbc` under SD `/gbc_cheats/`.
2. In-game press **`c`** → enable codes (they load **off** by default for safety).
3. Details: [`CHEATS.md`](CHEATS.md).

## License

Follow upstream project licensing in this repository. This fork does not grant rights to copyrighted ROMs or game content — supply your own legally obtained dumps.
