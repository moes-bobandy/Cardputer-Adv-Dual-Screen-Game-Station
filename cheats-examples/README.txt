Polished Crystal cheats for Game Station (GBC GameShark .cht)
==============================================================

Version: Polished Crystal **3.2.3** only (built from official .sym).
Do NOT use on 3.1.x / 3.2.0–3.2.2 / other hacks — can corrupt saves.

Install
-------
1. Backup your .sav
2. Copy the matching .cht onto the Cardputer SD as:
     /gbc_cheats/<EXACT-ROM-BASENAME>.cht
   Examples:
     polishedcrystal-3.2.3.gbc           -> polishedcrystal-3.2.3.cht
     polishedcrystal-faithful-3.2.3.gbc  -> polishedcrystal-faithful-3.2.3.cht
3. Launch the ROM on Game Station, press **c**, enable only the codes you want.

Notes
-----
- All codes start disabled (also firmware default).
- Bag codes overwrite **slot 1 of the matching pocket** only (that pocket needs
  at least one item already; the slot-1 item is replaced). See BAG section.
- Each '+' part of a code is its own row in the `c` menu (#1, #2, ...): turn on
  ALL rows of one cheat. Never use "Enable all".
- Party codes affect **party slot 1** — swap Pokémon to the front to apply.
- Prefer enabling Max Money OR bag items, not everything at once.
- Official stance: cheats unsupported; Polished Editor is safer for deep edits:
  https://polishededitor.vercel.app/

Shiny encounter (cheat13) — every wild encounter, not 1/N
----------------------------------------------------------
WHY 01A012D2 DID NOTHING (natural 1/4096 still won):
  TryAddMonToParty rolls shininess inside LoadEnemyWildmon and stores it on
  the wild mon, which lives in the OT party — wOTPartyMon1Personality /
  wOTPartyMon1Shiny @ D29F (3.2.3.sym, WRAM bank 1). SendInUserPkmn then
  copies that byte onto wEnemyMonPersonality @ D212 in the SAME stretch,
  before the next DelayFrame. A per-frame 01 write to D212 is either stale
  (pre-roll) or lands only after the sprite/sparkle have already sampled D29F.
  ClearBattleRAM also zeros D212 before the roll. Catch copies the whole
  wOTPartyMon1 struct (engine catch path), not the battle copy at D212.

  The byte the battle sprite and the shiny check actually read is D29F:
    GetEnemyFrontpicPalettePointer → GetEnemyMonPersonality → D29F
      (SetBattlePal_Enemy / CGB_BATTLE_COLORS)
    BattleStartMessage → BattleCheckEnemyShininess → GetEnemyMonPersonality
  wCurOTMon is 0 for a wild battle, so that is wOTPartyMon1Personality.

WRITE WINDOW (same GS timing as wild-force; no firmware change):
  LoadEnemyWildmon / TryAddMonToParty roll D29F, then InitBattleDisplay's
  ApplyTilemapInVBlank (DelayFrame) yields. Game Station applies 01/91 once
  per frame while the CPU is in that wait, and DoBattleTransition has already
  left rWBK = BANK(wEnemyMon) = 1 (D29F is 01:d29f, same bank). The write
  therefore sticks. Only AFTER that yield does CGB_BATTLE_COLORS load the
  frontpic palette and BattleStartMessage play the sparkle. Later frames
  keep D29F forced, so the catch copy is shiny too.

- Code: 01A09FD2  (0xA0 → D29F).
- SHINY_MASK = bit 7. Same byte also holds ABILITY_MASK (bits 5–6) and
  NATURE_MASK (bits 0–4). Gender/form are the NEXT byte (D2A0) and are not
  touched. GS types 01/91 are full-byte writes — no OR — so 0xA0 is
  SHINY_MASK|ABILITY_1|HARDY (nature 0). Ability and nature of the caught
  mon are replaced. Unavoidable without a bit-OR.
- In-battle ability was already cached from the pre-yield D212 copy
  (ResetEnemyAbility). The battle may still use the rolled ability; the
  mon you catch is Ability 1 + Hardy.
- Enable BEFORE walking into grass/water (not mid-battle: the palette is
  loaded once). Several encounters in a row should all sparkle and show
  the shiny palette. Disable after you are done.
- Trainer lead is the same D29F byte, so a trainer's first mon can also
  come out shiny. Other party slots are not covered.

Mart Rare Candy / Master Ball (cheat14 / cheat15)
-------------------------------------------------
- Patch wCurMart+1 @ D0F9 (item id) and wMartItem1BCD @ D002–D004 (list price).
- Rare Candy id 0x30, list price BCD 010000 (¥10000; matches ItemAttributes).
- Master Ball id 0x04, attribute price 0 → free in shop.
- Enable ONLY while inside a shop **Buy** menu. wCurMart / wMartItem1BCD sit in
  a heavily aliased WRAM region (radio, bug contest, mailbox, elevator, …).
- Do not enable cheat14 and cheat15 at the same time (both replace 1st item).
- Bag “Rare Candy x99” / “Master Ball x99” cheats are separate (instant bag write).
- The list price bytes are display only; the real charge comes from
  ItemAttributes (Rare Candy ¥10000, Max Potion ¥2500, Full Restore ¥3000,
  Master Ball ¥0). Purchases go through ReceiveItem → always the right pocket.
- Row 1 of Cherrygrove (pre-Dex) and Goldenrod 2F Mart 1 is POTION: while a
  mart code is on, that row is NOT a Potion any more. Turn it off to buy Potions.
- Buying >=10 of any ball also gives bonus Premier Balls (vanilla behaviour).
- cheat43 Max Potion / cheat44 Full Restore: same row-1 method (2026-10-05).


BAG POCKETS / ITEM CODES — fixed 2026-10-05 (Dirt: dual Master Ball, missing Potions)
--------------------------------------------------------------------------------------
Pocket layout (polishedcrystal-3.2.3.sym, ram/wramx.asm, WRAM bank 1). Each pocket
is: count byte, then (item id, quantity) pairs, then FF terminator.
  Item Pocket   wNumItems    D827   slot1 D828/D829   MAX_ITEMS    75
UPDATED 2026-10-06 ItemPkt REBUILD / Med SET count byte
  - Ghost 'Park Ball x0' in Items = NO_ITEM (id 00) qty 0; Master Ball in Items is stuck.
  - FIX: enable all rows of 'FIX ItemPkt REBUILD -> Repel x1', open Items, disable, SAVE.
  - Med/Ball/Item give codes are now SETs (count=1 + id + qty + FF); look in the RIGHT pocket.
  - Potions appear under Med.Pocket, never Items.

  Med.Pocket    wNumMedicine D8BF   slot1 D8C0/D8C1   MAX_MEDICINE 37
  Ball Pocket   wNumBalls    D90B   slot1 D90C/D90D   MAX_BALLS    25
  Berry Pocket  wNumBerries  D93F   slot1 D940/D941   MAX_BERRIES  31
  TM/HM = wTMsHMs flags D7F5;  Key Items = wKeyItems D800 (1 byte each)

Which pocket an item belongs in is FIXED by ItemAttributes (data/items/attributes.asm):
  BALL     Poke Ball 01, Great 02, Ultra 03, Master Ball 04 ...
  MEDICINE Potion 19, Super 1A, Hyper 1B, Max Potion 1C, Full Heal 22,
           Full Restore 23, Revive 24, Max Revive 25, Rare Candy 30 ...
  ITEM     Repel 5D, Max Repel 5F, Escape Rope 60, Nugget E5, Big Nugget E6 ...
Potions are in the Med.Pocket in PC 3.2.3, not the Item Pocket.

ROOT CAUSE of the old bug:
  Old cheat2 "Rare Candy x99" and cheat3 "Master Ball x99" wrote into Item
  Pocket slot 1 (D828). The game puts and finds items by their pocket:
  _ReceiveItem/_TossItem/_CheckItem/_CountItem (engine/items/items.asm) pass
  wNumItems and are sent to the item's own pocket. So:
   - Real Master Balls (free mart purchase, gifts) correctly go to the Ball
     Pocket → Master Ball shows in BOTH pockets.
   - The Item Pocket copy is invisible to game logic: Toss / Sell / Give / use
     in battle all act on the BALL pocket. Tossing it drains your real
     Ball-Pocket Master Balls (or fails) and the Item Pocket x99 stays.
   - If the Item Pocket was empty, the first item you then bought into it was
     written to D828 and overwritten by the cheat on the next frame (lost).
   - With a mart code on, shop row 1 (Potion in Cherrygrove / Goldenrod 2F)
     became Rare Candy / free Master Ball, so "buying Potions" actually bought
     Master Balls (x99 free, +9 Premier Balls) into the Ball Pocket.
  WHY IT SURVIVES codes-off + restart: GS codes only write live WRAM each frame.
  An in-game SAVE copies WRAM (bag included) into SRAM, which becomes the .sav.
  Codes load disabled on boot, but the item is now part of the save. It stays
  until something overwrites it.

Fixed codes (pocket-correct):
  cheat2  Med slot1: Rare Candy x99    0130C0D8+0163C1D8
  cheat3  Ball slot1: Master Ball x99  01040CD9+01630DD9
  cheat4-7 Item slot1: Nugget / Big Nugget / Max Repel / Escape Rope (D828, unchanged codes)
  cheat37 Med slot1: Max Potion x99    011CC0D8+0163C1D8
  cheat38 Med slot1: Hyper Potion x99  011BC0D8+0163C1D8
  cheat39 Med slot1: Full Restore x99  0123C0D8+0163C1D8
  cheat40 Med slot1: Max Revive x99    0125C0D8+0163C1D8
  cheat41 Med slot1: Full Heal x99     0122C0D8+0163C1D8
  cheat42 Ball slot1: Ultra Ball x99   01030CD9+01630DD9
Use: the pocket must already hold >=1 item (buy one cheap item into it). The
slot-1 item of that pocket is REPLACED, so put a throwaway item at the top
first (SELECT moves items). Turn on both rows, wait ~1 s, open the bag to check,
turn both rows OFF, then save. One slot code per pocket at a time (all
Med codes share D8C0, all Ball codes share D90C, all Item codes share D828).
Don't write an item that is already in that pocket lower down (duplicate stacks).

RECOVERY — stuck Master Ball x99 in the Item Pocket (keeps all progress):
  1. Back up the .sav. Open Pack → Item Pocket. The bad Master Ball should be the
     top row (old code wrote slot 1). If not, SELECT-move it to the top.
     Do NOT Toss it there (that would remove Ball-Pocket balls).
  2. `c` → turn ON both rows of "FIX ItemPkt slot1 MBall->Repel x1" (cheat35:
     015D28D8+010129D8). Wait ~1 s. Turn both rows OFF.
  3. Item Pocket top row is now Repel x1 (a proper Item-Pocket item). Toss it
     normally or keep it. The pocket count stays correct; nothing else moves.
  4. Ball Pocket Master Ball stack (if you don't want it): it is a legit item,
     just Toss it from the Ball Pocket. Only if that fails and it is the
     top Ball row, use cheat36 "FIX BallPkt slot1 -> Poke Ball x1"
     (01010CD9+01010DD9) the same way.
  5. Save in-game. The fix is permanent from then on.
  Same cheat35 also cleans an old Item-Pocket Rare Candy (old cheat2).

All TMs + HMs (cheat16) — non-glitchy / no menu freeze
------------------------------------------------------
- Symbol: wTMsHMs @ WRAM D7F5 … wTMsHMsEnd @ D800 (11 bytes).
- PC 3.2.3: flag_array NUM_TMS+NUM_HMS = 75 TMs + 6 HMs = 81 ownership bits
  (ReceiveTMHM / CheckTMHM use FlagAction on this array — not bag slots).
- Codes write 0xFF to D7F5–D7FE and 0x01 to D7FF (HM06 only in bit0).
  Spare bits 81–87 in the last byte MUST stay 0: CountTMsHMs counts every set
  bit in the array, and overcounting shifts CANCEL / scroll and can freeze the
  TM/HM pocket. Do NOT use an all-0xFF fill.
- Intentionally NOT stuffing TMs into Items/Medicine/etc. bag slots (that path
  corrupts pocket counts and is a known freeze/glitch risk).
- Tutors (MT) are NOT included — they are not stored in wTMsHMs.
- Enable outside battle, open Pack → TM/HM pocket to verify; disable when done
  (flags persist in save once written + saved).

Wild-force catch (cheat17–34) — SAFE fixed-species path
-------------------------------------------------------
Dirt requests: Catch Marill / Mew / Mewtwo / starters; “catch legendaries”.

WHY THE OLD D234 CODES DID NOTHING (fixed here; no firmware change):
  Game Station applies 01/91 writes once per frame, to whatever WRAM bank
  rWBK ($FF70) currently maps at D000–DFFF. That part is fine for this path.

  ChooseWildEncounter writes the rolled species to wTempWildMonSpecies (D234)
  and the form to wWildMonForm (D467), then the wild script calls startbattle
  in the SAME frame. BattleIntro's LoadTrainerOrWildMonPic immediately copies
  D234 → wTempEnemyMonSpecies (D22E) and D467 → wTempEnemyMonForm (D22F).
  A per-frame write cannot land between that store and that copy.

  The battle transition then DelayFrame's for many frames. Only AFTER that
  does InitEnemy → LoadEnemyWildmon read wTempEnemyMonSpecies (D22E) and
  GenerateWildForm read wWildMonForm (D467) to build the catchable mon.
  DoBattleTransition sets rWBK to BANK(wEnemyMon) = 1 (sym: 01:d22e) right
  before its last DelayFrame, so the between-frame cheat write hits WRAM
  bank 1, which is the bank those labels live in. Writing D234 during the
  transition is ignored: the snapshot in D22E was already taken.

  Species IDs and PLAIN_FORM were already correct (constants/pokemon_constants.asm
  + FORM_MASK %00011111). PLAIN_FORM = 1 is nonzero, so GenerateWildForm keeps
  the byte instead of re-rolling Unown/Magikarp.

Addresses (3.2.3.sym, GS-valid A000–DFFF, WRAM bank 1):
  wTempEnemyMonSpecies D22E   → 01xx2ED2   (what LoadEnemyWildmon reads)
  wWildMonForm         D467   → 01xx67D4   (what GenerateWildForm reads)
  wTempWildMonSpecies  D234   is NOT the force target (pre-snapshot only;
                               also aliased as wBT_TrainerTextIndex).

Flow (PC wildmons → battle):
  ChooseWildEncounter writes species→D234 and form→D467.
  LoadTrainerOrWildMonPic copies those into wTempEnemyMon* (same frame).
  Transition yields. Cheat overwrites D22E + D467 while bank 1 is mapped.
  LoadEnemyWildmon / GenerateWildForm then builds the full wild mon
  (base data, moves, stats, catch rate, nickname) once from those temps.
  Catch copies that built struct — not a live mid-battle species byte.

Why this is the SAFE force (vs mid-battle overwrite):
  - Species + form are forced TOGETHER on the temps LoadEnemyWildmon consumes.
  - GenerateWildForm: if (wWildMonForm & FORM_MASK) != 0, it keeps that byte
    (no Unown/Magikarp re-roll path). PLAIN_FORM = 1 → form byte 0x01.
  - All delivered targets are species < 0x100 → EXTSPECIES bit stays 0.
  - Magikarp length re-loop / Unown unlock re-roll do NOT apply to these IDs.
  - Mid-battle overwrite of wEnemyMonSpecies/Form (D209/D213) without rebuild
    remains UNSAFE (stats/moves/types desync) — not used here.

Delivered codes (enable=false; enable ONE while hunting wild grass/water):
  cheat17 Marill      species 0xB7 + PLAIN_FORM  → 01B72ED2+010167D4
  cheat18 Mew         species 0x97 + PLAIN_FORM  → 01972ED2+010167D4
  cheat19 Mewtwo      species 0x96 + PLAIN_FORM  → 01962ED2+010167D4
  cheat20 Bulbasaur   species 0x01 + PLAIN_FORM  → 01012ED2+010167D4
  cheat21 Charmander  species 0x04 + PLAIN_FORM  → 01042ED2+010167D4
  cheat22 Squirtle    species 0x07 + PLAIN_FORM  → 01072ED2+010167D4
  cheat23 Lugia       species 0xF9 + PLAIN_FORM  → 01F92ED2+010167D4
  cheat24 Ho-Oh       species 0xFA + PLAIN_FORM  → 01FA2ED2+010167D4
  cheat25 Articuno    species 0x90 + PLAIN_FORM  → 01902ED2+010167D4
  cheat26 Zapdos      species 0x91 + PLAIN_FORM  → 01912ED2+010167D4
  cheat27 Moltres     species 0x92 + PLAIN_FORM  → 01922ED2+010167D4
  cheat28 Pichu       species 0xAC + PLAIN_FORM  → 01AC2ED2+010167D4
  cheat29 Eevee       species 0x85 + PLAIN_FORM  → 01852ED2+010167D4
  cheat30 Vaporeon    species 0x86 + PLAIN_FORM  → 01862ED2+010167D4
  cheat31 Jolteon     species 0x87 + PLAIN_FORM  → 01872ED2+010167D4
  cheat32 Flareon     species 0x88 + PLAIN_FORM  → 01882ED2+010167D4
  cheat33 Espeon      species 0xC4 + PLAIN_FORM  → 01C42ED2+010167D4
  cheat34 Umbreon     species 0xC5 + PLAIN_FORM  → 01C52ED2+010167D4

Species IDs are the 3.2.3 const values (const_def 1; comments in
constants/pokemon_constants.asm for the v3.2.3 tag that matches this .sym):
  ARTICUNO 0x90, ZAPDOS 0x91, MOLTRES 0x92, LUGIA 0xF9, HO_OH 0xFA,
  PICHU 0xAC, EEVEE 0x85, VAPOREON 0x86, JOLTEON 0x87, FLAREON 0x88,
  ESPEON 0xC4, UMBREON 0xC5.
All are < 0x100, so the EXTSPECIES bit stays 0. PLAIN_FORM = 1 (nonzero),
so GenerateWildForm keeps the form byte. These are the Red/plain birds
and the plain beasts, not Galarian birds (those need a non-PLAIN form).

When to enable / caveats:
  - Enable only while walking for a wild encounter; disable immediately after
    catch (or if you cancel). Leave only one wild-force cheat on at a time.
  - Every wild slot (grass/water/swarm/roamer path that stages via these temps)
    becomes that species while enabled — intentional.
  - Trainer battles use a different path (wOtherTrainerClass); do not rely on
    these codes for trainers.
  - Level stays whatever the map encounter rolled.
  - Do NOT combine with mid-battle species patches.
  - Backup .sav first. Untested edge cases (ghost maps, some battle types)
    possible; disable if anything looks wrong.

NOT included: single “catch all legendaries” cheat
--------------------------------------------------
Game Station accepts ONLY type 01/91 fixed byte writeb each frame.
No random, rotate, list, or conditional codes.

Each legendary is a different species id, so each needs its own
wTempEnemyMonSpecies + wWildMonForm pair (same pattern as cheat17–34).
One fixed write cannot cover “all legendaries”.

Already covered as individual forces: Mew (cheat18), Mewtwo (cheat19),
Lugia (cheat23), Ho-Oh (cheat24), Articuno (cheat25), Zapdos (cheat26),
Moltres (cheat27).
Still not a single code (same reason — one fixed species per write):
  Raikou 0xF3, Entei 0xF4, Suicune 0xF5, Celebi 0xFB.
Galarian birds / armored Mewtwo need a non-PLAIN form byte — not delivered.

NOT included: Pokémon randomizer catch (wild encounters)
--------------------------------------------------------
Dirt clarified: list item #2 = wild encounters only.

  Game Station accepts ONLY type 01/91 fixed byte writeb each frame.
  No random, increment, rotate, or conditional codes.

  Fixed write cannot re-roll a new valid species+form each encounter.
  “Cycle” likewise needs increment/rotate — not available.
  Mid-battle overwrite of D209/D213 without rebuilding the enemy mon is unsafe.

Conclusion: true randomizer still impossible with GS fixed writes.
Fixed single-species wild-force (above) is the supported alternative.

Symbols confirmed (polishedcrystal-3.2.3.sym)
---------------------------------------------
wMoney              D7E5 (3 bytes, max 9999999 = 98 96 7F)
wEnemyMonSpecies    D209
wEnemyMonShiny      D212  (battle copy; rolled AFTER a D212 GS write — do not use)
wOTPartyMon1Shiny   D29F  (sprite/sparkle/catch personality; shiny cheat)
wTempEnemyMonSpecies D22E  (wild-force species; LoadEnemyWildmon input)
wTempEnemyMonForm    D22F
wTempWildMonSpecies D234  (pre-snapshot only; do not use for wild-force)
wWildMonForm        D467  (wild-force form; GenerateWildForm input)
wCurMart            D0F8  (count); items at D0F9…
wMartItem1BCD       D002
wNumItems / wItems       D827 / D828  (Item Pocket, 75 slots)
wNumMedicine / wMedicine D8BF / D8C0  (Med.Pocket, 37 slots)
wNumBalls / wBalls       D90B / D90C  (Ball Pocket, 25 slots)
wTMsHMs            D7F5 (11 bytes; 75 TM + 6 HM flags; last byte bit0=HM06)
