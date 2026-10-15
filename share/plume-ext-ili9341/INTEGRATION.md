# EXT ILI9341 on Cardputer ADV — known-good reference (from dual-screen Game Station)

Reference header: `LGFX_ILI9341_reference.h` (verbatim from the shipping dual-screen build).

## Pins (PorkChop-style 2.8" ILI9341)
SCK 40, MOSI 14, CS 5, DC 6, RST 3. MISO -1 for the panel bus. SD_CS 12 shares SCK/MOSI.

## Bus
SPI3_HOST, mode 0, freq_write 40 MHz, freq_read 16 MHz, spi_3wire true, use_lock true, dma_channel 1.
Panel: bus_shared true, readable false, invert false, rgb_order false, dlen_16bit false,
240x320 memory/panel, offset_rotation 4.

## Init order that works
1. `M5Cardputer.begin(cfg)` with `cfg.output_power = true`
2. `ext.setColorDepth(16); ext.begin(); delay(50); ext.setRotation(3); lcd_boot_rgb565();`
3. SD `begin()` (same SCK/MOSI)
4. `lcd_resume_after_bus()` = LCD_CS HIGH + 2ms. No COLMOD, no setColorDepth.

## Backlight-on / no-pixels checklist
- Wrong SPI host (must be SPI3; internal ST7789 keeps its own bus)
- CS left LOW by SD or another driver
- Missing SLPOUT / DISPON in the init list
- spi_3wire mismatch
- Re-sending COLMOD (0x3A) after begin, or mid-run `setColorDepth`

## Shared-bus rules (hard-won)
- COLMOD 0x55 lives in the panel init list only.
- Boot only: `setColorDepth(16)` + `setSwapBytes(true)`.
- Never `writeCommand(0x3A)` after the render task starts — races startWrite and freezes.
- Before any SD access: wait for the render task to leave startWrite, then endWrite/waitDisplay/CS HIGH.
- After SD: release CS only.

## Scaling notes (if Plume renders a small framebuffer)
- Integer/Bresenham maps beat per-pixel float; uneven row duplication looks like a "wave" in motion.
- Keep a latest-frame-only queue instead of a backlog.
- Keep the render task priority BELOW audio if audio shares a core.
