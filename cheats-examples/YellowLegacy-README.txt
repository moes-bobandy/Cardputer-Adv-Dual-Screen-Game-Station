Pokemon Yellow Legacy — Game Station .cht
=========================================

Exact file Dirt must match
--------------------------
  ROM:  PokemonYellowLegacy.gbc
  CHT:  PokemonYellowLegacy.cht
  SD:   /gbc_cheats/PokemonYellowLegacy.cht

Paths written
-------------
  /workspace/yellow-legacy-cheats/PokemonYellowLegacy.cht
  /workspace/yellow-legacy-cheats/pokeyellow-legacy.sym
  /workspace/yellow-legacy-cheats/README.txt
  /workspace/polished-crystal-cheats/PokemonYellowLegacy.cht
  /workspace/gba-dual-screen-fix/cheats-examples/PokemonYellowLegacy.cht

Alternative basenames (only if ROM is renamed)
----------------------------------------------
  Pokemon Yellow Legacy.cht
  PokemonYellowLegacy1.0.10.cht
  pokeyellow.cht   (Makefile default — NOT Dirt's patched name)

Address source (no invented addresses)
--------------------------------------
  RGBDS 0.6.1 build of cRz-Shadows/Pokemon_Yellow_Legacy → pokeyellow.sym
  Key labels verified in that sym (see .cht header).

Running shoes
-------------
  ALREADY IN THE HACK: hold B to sprint (home/overworld.asm).
  NOT a permanent always-on option/flag.
  No always-on GS cheat added: type 01/91 cannot OR B into hJoyHeld
  without wiping D-pad/A (breaks menus/battles). No dedicated WRAM flag.

Shiny every wild encounter
--------------------------
  SKIPPED — Yellow Legacy has no shiny system (Gen1; no personality/shiny
  mask like PC wOTPartyMon1Shiny @ D29F). Cannot mirror PC shiny cheat.

How bag / mart / wild-force differ from PC
------------------------------------------
  BAG: one Items pocket (D31C count, D31D/D31E slot1). No Med/Ball pockets.
       Capacity 59. All "Med slot" / "Ball slot" PC cheats map to Bag slot1.
  MART: wItemList+1 @ CF7B (item id list). Prices from ROM ItemPrices
        (Master Ball free). No separate wMartItem1BCD display patch needed.
  WILD: overwrite all 10 grass + 10 water SPECIES bytes in wGrassMons /
        wWaterMons (D888+ / D8A5+). Levels stay map-native. Enable one
        while hunting; off after catch. (wEnemyMonSpecies2 alone is too
        late for same-frame LoadEnemyMonData.)

PC feature → YL status (35 ported / rest skipped)
-------------------------------------------------
  Max Money                         PORTED  wPlayerMoney D394
  Max Coins                         PORTED  wPlayerCoins D5F1
  Med slot1 Rare Candy              PORTED  as Bag slot1
  Ball slot1 Master Ball            PORTED  as Bag slot1
  Item slot1 Nugget                 PORTED  Bag slot1
  Item slot1 Big Nugget             SKIPPED item does not exist
  Item slot1 Max Repel / EscapeRope PORTED  Bag slot1
  Party1 Max EVs                    PORTED  as Max StatExp (Gen1)
  Party1 Max DVs                    PORTED  2-byte DVs (not 3)
  Party1 Max Happiness              SKIPPED no party happiness byte
  Battle lead full HP               PORTED  wBattleMon HP/MaxHP
  Party1 Level 100                  PORTED  wPartyMon1Level D18B
  Shiny encounter                   SKIPPED no shiny system
  Mart Rare Candy / Master Ball     PORTED  wItemList+1 CF7B
  All TMs + HMs flags               SKIPPED no wTMsHMs flag array
  Wild Marill                       SKIPPED Gen2
  Wild Mew / Mewtwo / starters      PORTED  encounter-table force
  Wild Lugia / Ho-Oh                SKIPPED Gen2
  Wild Articuno / Zapdos / Moltres  PORTED
  Wild Pichu                        SKIPPED Gen2
  Wild Eevee + 3 Eeveelutions       PORTED  (no Espeon/Umbreon)
  Wild Espeon / Umbreon             SKIPPED Gen2
  FIX ItemPkt / BallPkt             PORTED  as single-bag FIX rows
  Med Max/Hyper/FullRestore/etc.    PORTED  as Bag slot1
  Ball Ultra                        PORTED  as Bag slot1
  Mart Max Potion / Full Restore    PORTED

  Always-run shoes                  NOT ADDED (hack already has B-hold;
                                    GS cannot safely force always-on)

Usage
-----
  Backup .sav. All cheats start enable=false. Press c, enable only what
  you need. Turn on ALL + parts of a multi-row cheat. Never "Enable all".
  Bag/mart/wild-force fight over the same WRAM — one at a time.
