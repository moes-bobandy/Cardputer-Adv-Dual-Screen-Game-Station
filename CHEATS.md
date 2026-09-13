# GameShark cheats (GB/GBC)

In-game GameShark-style memory cheats for the dual-screen Game Station GBC core.

## Hotkey

Press **`c`** during gameplay to open the **Cheats** menu (pauses the emulator).

Menu actions:
- **Enable all** / **Disable all**
- **Reload from SD**
- Toggle each loaded cheat (`[ON]` / `[OFF]`)
- **Back** (or left arrow / backtick)

## SD path

Place files on the SD card at:

```
/gbc_cheats/<ROM basename>.cht
```

(also accepts `.txt`)

Firmware mounts SD at `/sd`, so on-device path is `/sd/gbc_cheats/<basename>.cht`.

`<ROM basename>` is the ROM filename without directory or extension.
Example: ROM `Pokemon Card GB2.gbc` -> `/sd/gbc_cheats/Pokemon Card GB2.cht`

Cheats auto-load after SRAM load. Missing files are ignored.

**Default state:** codes are loaded but **all disabled** until you turn them on in the `c` menu. File `cheatN_enable` flags are ignored for auto-apply so a bad English-translation address cannot corrupt boot.

## Code format

8-hex GameShark: `01xxCDAB` or `91xxCDAB`
- Writes value `xx` to address `0xABCD` (CDAB little-endian)
- Example: `01FF9FD5` writes `0xFF` to `0xD59F`

## File formats

RetroArch `.cht`:

```
cheat0_desc = "All Coins"
cheat0_code = "01FF9FD5+01FFA0D5+01FFA1D5+010FA2D5"
cheat0_enable = true
```

Or bare 8-hex lines / codes joined with `+`. Up to **512** codes; applied every frame via `writeb` when enabled.

Only cartridge RAM / work RAM (`A000`–`DFFF`) and high RAM (`FF80`–`FFFE`) are accepted (e.g. `D59F` is valid).

## Example: Pokemon Card GB2

Small RetroArch pack — see `cheats-examples/Pokemon Card GB2.cht`:

| Cheat | Codes |
|-------|-------|
| All Coins | 01FF9FD5, 01FFA0D5, 01FFA1D5, 010FA2D5 |
| No Energy Card limit | 010004CC |

**Have All Cards** (445 bare GameShark lines, no All Coins):  
`cheats-examples/Pokemon Card GB2 - Have All Cards.cht` — copy to SD as  
`/gbc_cheats/Pokemon Card GB2.cht` matching your ROM basename. Requires the 512-slot firmware (v12.3+). Enable in the `c` menu, then save so SRAM keeps the cards.
