<p align="center">
<img width="840" height="500" alt="bg" src="https://github.com/user-attachments/assets/218f16a4-4bf1-4485-bd99-a5bd9fed0398" />
</p>

<h1 align="center">ReST-V</h1>

<p align="center">
  <b>Sega ST-V and System C / C2 arcade emulator for PlayStation Vita</b><br>
  Yabause based &middot; SH-2 dynarec for ARMv7 &middot; GPU VDP1/VDP2 rendering &middot; 174 sets<br>
  <a href="https://www.patreon.com/Reyanon">Support on Patreon</a> or <a href="https://ko-fi.com/reyanon">Ko-Fi</a>
</p>

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Highlights](#2-highlights)
3. [Supported Boards and Games](#3-supported-boards-and-games)
4. [Download](#4-download)
5. [Installation](#5-installation)
6. [Usage](#6-usage)
7. [Building from Source](#7-building-from-source)
8. [Project Layout](#8-project-layout)
9. [Accuracy and Testing](#9-accuracy-and-testing)
10. [License and Credits](#10-license-and-credits)
11. [Support](#11-support)

---

## 1. Introduction

ReST-V is an emulator for the Sega ST-V (Titan) arcade board, built on the SH-2, SCU, VDP1/VDP2, SCSP and SMPC
code of [Yabause / YabaSanshiro](https://github.com/libretro/yabause) with the ST-V support of
[Kronos](https://github.com/FCare/Kronos). It also runs the older Sega System C and System C2 boards with a
separate 68000 based core.

The ST-V is a Saturn-class machine: two SH-2 CPUs at 28.6 MHz, a 68000 for sound, two video chips and an
SCSP with its own DSP. The PlayStation Vita has a 444 MHz Cortex-A9, so a straight port of a desktop
emulator runs far below full speed. The goal of ReST-V is a locked 60 fps with clean audio on the Vita,
without frame skipping. Every frame is emulated and drawn.

Board accuracy is kept where the player can see or hear it. Timing details that do not change what is on
screen are simplified when that buys speed.

## 2. Highlights

**ST-V core**
- SH-2 dynamic recompiler for ARMv7 (derived from the devmiyax recompiler in YabaSanshiro) with block
  chaining, fast work-RAM access from generated code and a large code cache.
- Automatic idle-loop detection (SGL `slSynch`, `bra .` loops, stack-variable delay loops) and per-game
  hints, so waiting CPUs do not burn host time.
- Native (HLE) versions of hot SGL library routines: vertex transform, polygon loops, fixed-point division,
  memcpy/memset family. Each one is checked against the original code with a differential tester.
- SCSP DSP compiled to ARM code; 68000 and SCSP on their own core.
- 315-5881 protection (Tecmo World Cup '98 and others) ported from MAME.

**Rendering (sceGxm)**
- VDP1 command lists drawn on the GPU with a sprite texture cache, including shadow, half-luminance,
  half-transparency and MSB-on modes.
- VDP2 NBG layers drawn as cell quads from a character atlas, bitmap layers as textures, RBG0 rotation as
  per-line GPU quads.
- Priority, color calculation and windows composited in a shader specialized per frame setup; variants are
  compiled at runtime and cached on the memory card.
- Layers that did not change are not drawn again (this is not frame skipping; the output is the same).

**System C / C2 core**
- Musashi 68000, a line-based 315-5313 VDP renderer, YM3438 (ymfm), SN76489 and uPD7759.
- Board I/O, external palette and the protection chips of the System C2 sets.

**Frontend**
- Game list with System C / System C2 / ST-V tabs and a Playable / All / Favorites filter.
- Save states (10 slots plus quick save), button mapping with turbo, analog stick as lever, built-in cheats.
- Display options: aspect ratio, filter, scanlines, border color or user artwork.
- Per-game settings: save the current display and control settings for one game only.

## 3. Supported Boards and Games

Set names are the MAME set names. ROMs must match the MAME romset; clone sets read missing files from the
parent zip. ST-V games also need the BIOS set `stvbios.zip`.

**Performance**: *Playable*, *Partial* or *Not working*, and the average frame rate on a PS Vita at 444 MHz
over a few minutes of attract mode and play. Full speed is 60 fps (59.94 Hz). A rate with `~` is estimated from
tested games on the same board, not measured for that set. No rate means the set has not been measured.
*Partial* sets run, but MAME also lists them as not working (missing printer, camera or other hardware), or a
known problem remains. Click a board to expand its list.

### PS Vita (444 MHz)

<details>
<summary><b>ST-V</b> (117 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `znpwfvt` | `znpwfv` | 1997 | All Japan Pro-Wrestling Featuring Virtua (T 971123 V1.000) | Playable<br>55.1 fps |
| `aclub` |  | 1997 | Aroma Club (J 970611 V1.000) | Playable |
| `astrass` |  | 1998 | Astra SuperStars (J 980514 V1.002) | Not working<br>34.1 fps |
| `bakubaku` |  | 1995 | Baku Baku Animal (J 950407 V1.000) | Playable<br>60 fps |
| `batmanfr` |  | 1996 | Batman Forever (JUE 960507 V1.000) | Playable<br>54.7 fps |
| `chalgolf` |  | 1999 | Challenge Golf (J 990326 V1.000) | Playable |
| `choroqhr` |  | 1998 | Choro Q Hyper Racing 5 (J 981230 V1.000) | Playable<br>59.8 fps |
| `colmns97` |  | 1996 | Columns '97 (JET 961209 V1.000) | Playable<br>60 fps |
| `cotton2` |  | 1997 | Cotton 2 (JUET 970902 V1.000) | Playable<br>60 fps |
| `cottonbm` |  | 1998 | Cotton Boomerang (JUET 980709 V1.000) | Playable<br>59.9 fps |
| `sanjeon` | `sasissu` | 1999 | DaeJeon! SanJeon SuJeon (AJTUE 990412 V1.000) | Playable<br>60 fps |
| `danchih` |  | 1999 | Danchi de Hanafuda (J 990607 V1.400) | Playable<br>60 fps |
| `danchiq` |  | 2000 | Danchi de Quiz: Okusan Yontaku Desuyo! (J 001128 V1.200) | Playable<br>60 fps |
| `dfeverg` |  | 2001 | Dancing Fever Gold (J 000821 V2.001) | Partial<br>49.6 fps |
| `decathlto` | `decathlt` | 1996 | DecAthlete (JUET 960424 V1.000) | Partial<br>57.3 fps |
| `decathlt` |  | 1996 | DecAthlete (JUET 960709 V1.001) | Partial<br>57.3 fps |
| `diehard` |  | 1996 | Die Hard Arcade (UET 960515 V1.000) | Playable<br>60 fps |
| `dnmtdeka` | `diehard` | 1996 | Dynamite Deka (J 960515 V1.000) | Playable<br>60 fps |
| `ejihon` |  | 1995 | Ejihon Tantei Jimusho (J 950613 V1.000) | Playable<br>60 fps |
| `fanzonem` |  | 1999 | Fantasy Zone (medal game, REV.A) (J 990202 V1.000) | Partial<br>60 fps |
| `finlarch` | `smleague` | 1995 | Final Arch (J 950714 V1.001) | Playable<br>58.9 fps |
| `ffrevng10` | `ffreveng` | 1999 | Final Fight Revenge / Final Revenge (JUET 990714 V1.000) | Playable<br>60 fps |
| `ffreveng` |  | 1999 | Final Fight Revenge / Final Revenge (JUET 990930 V1.100) | Playable<br>60 fps |
| `fhboxers` |  | 1995 | Funky Head Boxers (JUETBKAL 951218 V1.000) | Playable<br>55.2 fps |
| `gaxeduel` |  | 1994 | Golden Axe - The Duel (JUETL 950117 V1.000) | Playable<br>59.6 fps |
| `groovef` |  | 1997 | Groove on Fight - Gouketsuji Ichizoku 3 (J 970416 V1.001) | Playable<br>60 fps |
| `grdforce` |  | 1998 | Guardian Force (JUET 980318 V0.105) | Playable<br>60 fps |
| `patocar` |  | 2001 | Hashire Patrol Car (J 990326 V1.000) | Playable<br>59.9 fps |
| `introdon` |  | 1996 | Karaoke Quiz Intro Don Don! (J 960213 V1.000) | Playable<br>59.2 fps |
| `magzun` |  | 1996 | Magical Zunou Power (J 961031 V1.000) | Partial<br>60 fps |
| `maruchan` |  | 1997 | Maru-Chan de Goo! (J 971216 V1.000) | Playable<br>59.2 fps |
| `mausuke` |  | 1996 | Mausuke no Ojama the World (J 960314 V1.000) | Playable<br>59.2 fps |
| `micrombc` |  | 1999 | Microman Battle Charge (J 990326 V1.000) | Playable<br>60 fps |
| `nameclub` |  | 1996 | Name Club (J 960315 V1.000) | Playable<br>60 fps |
| `nclubdis` |  | 1998 | Name Club Disney (J 980614 V1.000) | Partial<br>60 fps |
| `nclubv2` |  | 1996 | Name Club Ver.2 (J 960315 V1.000) | Playable<br>60 fps |
| `nclubv3` |  | 1997 | Name Club Ver.3 (J 970723 V1.000) | Playable<br>60 fps |
| `nclubv4` |  | 1997 | Name Club Ver.4 (J 971202 V1.000) | Playable<br>60 fps |
| `supgoal` |  | 1998 | Nerae! Super Goal (J 981218 V1.000) | Playable<br>59.3 fps |
| `othellos` |  | 1998 | Othello Shiyouyo (J 980423 V1.002) | Playable<br>60 fps |
| `pblbeach` |  | 1995 | Pebble Beach - The Great Shot (JUE 950913 V0.990) | Playable<br>59.2 fps |
| `prikura` |  | 1996 | Princess Clara Daisakusen (J 960910 V1.000) | Partial |
| `prc297wi` |  | 1997 | Print Club 2 '97 Winter Ver (J 971017 V1.100, set 1) | Partial |
| `prc297wia` | `prc297wi` | 1997 | Print Club 2 '97 Winter Ver (J 971017 V1.100, set 2) | Partial |
| `prc298au` |  | 1998 | Print Club 2 '98 Autumn Ver (J 980827 V1.000) | Partial |
| `prc298sp` |  | 1998 | Print Club 2 '98 Spring Ver (J 971017 V1.100) | Partial |
| `prc298su` |  | 1998 | Print Club 2 '98 Summer Ver (J 980603 V1.100) | Partial |
| `prc2ksu` |  | 2000 | Print Club 2 2000 Summer (J 000509 V1.000) | Partial |
| `pclub2bb` |  | 1997 | Print Club 2 Banpresto Bono Bono (J 970925 V1.000) | Partial |
| `pclub2bu` |  | 1997 | Print Club 2 Banpresto Ultraman (J 970604 V1.000) | Partial |
| `pclb2elk` |  | 1997 | Print Club 2 Earth Limited Kobe (Print Club Custom) (J 970808 V1.000) | Partial |
| `pclub2ev` |  | 1997 | Print Club 2 Evangelion (J 970319 V1.000) | Partial |
| `pclub2fc` |  | 1999 | Print Club 2 Felix The Cat (Rev. A) (J 970415 V1.100) | Partial |
| `pckobe99` |  | 1997 | Print Club 2 Kobe Luminaire '99 (Print Club Custom 3) (J 991203 V1.000) | Partial<br>60 fps |
| `pclub2lc` |  | 1999 | Print Club 2 Lamb Chop and Friends (J 990520 V1.000) | Partial |
| `pclub2mb` |  | 1998 | Print Club 2 Mr. Bean (J 980325 V1.000) | Partial |
| `pclub2pe` |  | 1997 | Print Club 2 Pepsiman (J 970618 V1.100) | Partial |
| `pclub2pf` |  | 1998 | Print Club 2 Puffy (J V1.100) | Partial |
| `pclub2` |  | 1997 | Print Club 2 Standard Ver1 (U 970921 V1.000) | Partial |
| `pclub2ts` |  | 1997 | Print Club 2 Tsubuyaki Shiro (J 970708 V1.100) | Partial |
| `pclub2v3` |  | 1999 | Print Club 2 Vol. 3 (U 990310 V1.000) | Partial |
| `pclub26w` |  | 1997 | Print Club 2 Vol. 6 Winter (J 961210 V1.000) | Partial |
| `pclub26wa` | `pclub26w` | 1997 | Print Club 2 Vol. 6 Winter (J 970121 V1.200) | Partial |
| `pclub27s` |  | 1997 | Print Club 2 Vol. 7 Spring (J 970313 V1.100) | Partial |
| `prc28su` |  | 1997 | Print Club 2 Vol. 8 '97 Summer (J 970616 V1.100) | Partial |
| `prc29au` |  | 1997 | Print Club 2 Vol. 9 Autumn (J V1.100) | Partial |
| `pclub2wb` |  | 1997 | Print Club 2 Warner Bros (J 970228 V1.000) | Partial |
| `pclubor` |  | 1999 | Print Club Goukakenran (J 991104 V1.000) | Partial |
| `pclub2kc` |  | 1997 | Print Club Kome Kome Club (J 970203 V1.000) | Partial |
| `pclove` |  | 1997 | Print Club LoveLove (J 970421 V1.000) | Partial |
| `pclove2` |  | 1997 | Print Club LoveLove Ver 2 (J 970825 V1.000) | Partial |
| `pclubnbc` |  | 1998 | Print Club Nightmare Before Christmas (J 980717 V1.000) | Partial |
| `pclubol` |  | 1999 | Print Club Olive (J 980717 V1.000) | Partial |
| `pclubpok` |  | 1999 | Print Club Pokemon B (U 991126 V1.000) | Partial |
| `pclubsc5` |  | 1998 | Print Club Sony Creative Ver.5 (J 980721 V1.000) | Partial |
| `pclubsc6` |  | 1997 | Print Club Sony Creative Ver.6 (J 971006 V1.000) | Partial |
| `pcpooh2` |  | 1997 | Print Club Winnie-the-Pooh Vol. 2 (J 971218 V1.000) | Partial |
| `pcpooh3` |  | 1998 | Print Club Winnie-the-Pooh Vol. 3 (J 980406 V1.000) | Partial |
| `pclubyo` |  | 1997 | Print Club Yoshimoto V1 (J 970208 V1.000) | Partial |
| `pclubyo2` |  | 1997 | Print Club Yoshimoto V2 (J 970422 V1.100) | Partial |
| `kiwames` |  | 1995 | Pro Mahjong Kiwame S (J 951020 V1.208) | Playable<br>56.4 fps |
| `puyosun` |  | 1996 | Puyo Puyo Sun (J 961115 V0.001) | Playable<br>59.0 fps |
| `thuntk` | `sandor` | 1997 | Puzzle & Action: BoMulEul Chajara (JUET 970125 V2.00K) | Playable<br>60 fps |
| `sandor` |  | 1995 | Puzzle & Action: Sand-R (J 951114 V1.000) | Playable<br>60 fps |
| `thunt` | `sandor` | 1997 | Puzzle & Action: Treasure Hunt (JUET 970901 V2.00E) | Playable<br>60 fps |
| `rsgun` |  | 1998 | Radiant Silvergun (JUET 980523 V1.000) | Playable<br>60 fps |
| `hanagumi` |  | 1997 | Sakura Taisen - Hanagumi Taisen Columns (J 971007 V1.010) | Playable<br>58.9 fps |
| `seabass` |  | 1997 | Sea Bass Fishing (JUET 971110 V0.001) | Playable<br>59.4 fps |
| `shanhigw` |  | 1995 | Shanghai - The Great Wall / Shanghai Triple Threat (JUE 950623 V1.005) | Playable<br>59.5 fps |
| `shienryu` |  | 1997 | Shienryu (JUET 961226 V1.000) | Playable<br>60 fps |
| `pclub2sr` |  | 1998 | Showa Retro Print Club 2 (J 980217 V1.000) | Partial |
| `skychal` |  | 2000 | Sky Challenger (J 000406 V1.000) | Playable<br>60 fps |
| `slotbatt` |  | 2002 | Slot Battler (J 020703 V1.000) | Playable |
| `sackids` |  | 2000 | Soreyuke Anpanman Crayon Kids (J 001026 V1.000) | Partial<br>60 fps |
| `sokyugrt` |  | 1996 | Soukyugurentai / Terra Diver (JUET 960821 V1.000) | Playable<br>59.1 fps |
| `sfish2j` | `sfish2` | 1995 | Sport Fishing 2 (J 951201 V1.100) | Partial<br>60 fps |
| `sfish2` |  | 1995 | Sport Fishing 2 (UET 951106 V1.10e) | Partial<br>60 fps |
| `sss` |  | 1998 | Steep Slope Sliders (JUET 981110 V1.000) | Playable<br>60 fps |
| `stress` |  | 1998 | Stress Busters (J 981020 V1.000) | Partial<br>60 fps |
| `suikoenb` |  | 1995 | Suiko Enbu / Outlaws of the Lost Dynasty (JUETL 950314 V2.001) | Playable<br>60 fps |
| `smleague` |  | 1995 | Super Major League (U 960108 V1.000) | Partial<br>59.2 fps |
| `sasissu` |  | 1998 | Taisen Tanto-R Sashissu!! (J 980216 V1.000) | Playable<br>60 fps |
| `techbowl` |  | 1997 | Technical Bowling (J 971212 V1.000) | Playable<br>57.9 fps |
| `twcup98` |  | 1998 | Tecmo World Cup '98 (JUET 980410 V1.000) | Playable<br>58.9 fps |
| `twsoc98` | `twcup98` | 1998 | Tecmo World Soccer '98 (JUET 980410 V1.000) | Playable<br>57.7 fps |
| `elandore` |  | 1998 | Touryuu Densetsu Elan-Doree / Elan Doree - Legend of Dragoon (JUET 980922 V1.006) | Playable<br>55.3 fps |
| `tsuribor` |  | 1998 | Tsuribori Taikai (JAE 980605 V1.000) | Playable |
| `vfkids` |  | 1996 | Virtua Fighter Kids (JUET 960319 V0.000) | Playable<br>57.5 fps |
| `vfremix` |  | 1995 | Virtua Fighter Remix (JUETBKAL 950428 V1.000) | Playable<br>57.6 fps |
| `vmahjong` |  | 1997 | Virtual Mahjong (J 961214 V1.000) | Playable<br>57.4 fps |
| `myfairld` |  | 1998 | Virtual Mahjong 2 - My Fair Lady (J 980608 V1.000) | Playable<br>59.7 fps |
| `wwshin` |  | 1997 | Waku Waku Shinkansen (J 971031 V0.002) | Playable |
| `wasafari` |  | 1998 | Wanpaku Safari (J 981109 V1.000) | Playable<br>60 fps |
| `winterht` |  | 1997 | Winter Heat (JUET 971012 V1.000) | Playable<br>57.8 fps |
| `yattrmnp` |  | 1998 | Yatterman Plus (J 981006 V1.000) | Partial<br>60 fps |
| `znpwfv` |  | 1997 | Zen Nippon Pro-Wres Featuring Virtua (J 971123 V1.000) | Playable<br>57.5 fps |
| `findlove` |  | 1997 | Zenkoku Seifuku Bishoujo Grand Prix Find Love (J 971212 V1.000) | Playable<br>59.1 fps |

</details>

<details>
<summary><b>System C2</b> (47 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `borenchj` | `borench` | 1990 | Borench (Japan) | Playable<br>~60 fps |
| `borench` |  | 1990 | Borench (set 1) | Playable<br>~60 fps |
| `borencha` | `borench` | 1990 | Borench (set 2) | Playable<br>~60 fps |
| `headonch` |  | 1994 | Monita to Rimoko no Head On Channel (prototype, hack) | Playable<br>~60 fps |
| `ooparts` |  | 1992 | OOPArts (prototype, joystick hack) | Playable<br>~60 fps |
| `potopoto` |  | 1994 | Poto Poto (Japan, Rev A) | Playable<br>~60 fps |
| `pclubj` |  | 1995 | Print Club (Japan Vol.1) | Partial<br>~60 fps |
| `pclubjv2` |  | 1995 | Print Club (Japan Vol.2) | Partial<br>~60 fps |
| `pclubjv3` |  | 1995 | Print Club (Japan Vol.3) | Not working |
| `pclubjv4` |  | 1996 | Print Club (Japan Vol.4) | Partial<br>~60 fps |
| `pclubjv5` |  | 1996 | Print Club (Japan Vol.5) | Partial<br>~60 fps |
| `pclub` | `pclubjv2` | 1995 | Print Club (World) | Partial<br>~60 fps |
| `puyoja` | `puyo` | 1992 | Puyo Puyo (Japan, Rev A) | Playable<br>~60 fps |
| `puyoj` | `puyo` | 1992 | Puyo Puyo (Japan, Rev B) | Playable<br>~60 fps |
| `puyo` |  | 1992 | Puyo Puyo (World) | Playable<br>~60 fps |
| `puyobl` | `puyo` | 1992 | Puyo Puyo (World, bootleg) | Playable<br>~60 fps |
| `puyopuy2` |  | 1994 | Puyo Puyo 2 (Japan) | Playable<br>~60 fps |
| `ichirj` | `ichir` | 1994 | Puzzle & Action: Ichidant-R (Japan) | Playable<br>~60 fps |
| `ichirk` | `ichir` | 1994 | Puzzle & Action: Ichidant-R (Korea) | Playable<br>~60 fps |
| `ichir` |  | 1994 | Puzzle & Action: Ichidant-R (World) | Playable<br>~60 fps |
| `ichirbl` | `ichir` | 1994 | Puzzle & Action: Ichidant-R (World) (bootleg) | Playable<br>~60 fps |
| `tantr` |  | 1992 | Puzzle & Action: Tant-R (Japan) | Playable<br>~60 fps |
| `tantrbl` | `tantr` | 1992 | Puzzle & Action: Tant-R (Japan) (bootleg set 1) | Playable<br>~60 fps |
| `tantrbl4` | `tantr` | 1992 | Puzzle & Action: Tant-R (Japan) (bootleg set 4) | Playable<br>~60 fps |
| `tantrkor` | `tantr` | 1993 | Puzzle & Action: Tant-R (Korea) | Playable<br>~60 fps |
| `ribbit` |  | 1991 | Ribbit! | Playable<br>~60 fps |
| `ribbitj` | `ribbit` | 1991 | Ribbit! (Japan) | Playable<br>~60 fps |
| `ssonicbr` |  | 1992 | SegaSonic Bros. (prototype, hack) | Playable<br>~60 fps |
| `sonicfgtj` | `sonicfgt` | 1993 | SegaSonic Cosmo Fighter (Japan) | Playable<br>~60 fps |
| `sonicfgt` |  | 1993 | SegaSonic Cosmo Fighter (World) | Playable<br>~60 fps |
| `sonicpop` |  | 1993 | SegaSonic Popcorn Shop (Rev B) | Playable<br>~60 fps |
| `anpanmana` | `anpanman` | 1992 | Soreike! Anpanman Popcorn Koujou (Rev A) | Playable<br>~60 fps |
| `anpanman` |  | 1992 | Soreike! Anpanman Popcorn Koujou (Rev B) | Playable<br>~60 fps |
| `stkclmnsj` | `stkclmns` | 1994 | Stack Columns (Japan) | Playable<br>~60 fps |
| `stkclmns` |  | 1994 | Stack Columns (World) | Playable<br>~60 fps |
| `tfrceac` |  | 1990 | Thunder Force AC | Playable<br>~60 fps |
| `tfrceacb` | `tfrceac` | 1990 | Thunder Force AC (bootleg) | Playable<br>~60 fps |
| `tfrceacj` | `tfrceac` | 1990 | Thunder Force AC (Japan) | Playable<br>~60 fps |
| `tfrceacjpb` | `tfrceac` | 1990 | Thunder Force AC (Japan, prototype, bootleg) | Playable<br>~60 fps |
| `twinsqua` |  | 1991 | Twin Squash | Playable<br>~60 fps |
| `wwanpanmo` | `wwanpanm` | 1992 | Waku Waku Anpanman | Not working |
| `wwanpanm` |  | 1992 | Waku Waku Anpanman (Rev A) | Playable<br>~60 fps |
| `wwjumbo` |  | 1990 | Waku Waku Jumbo (Rev A) | Not working |
| `wwmarine` |  | 1992 | Waku Waku Marine | Playable<br>~60 fps |
| `wwpajero` |  | 1990 | Waku Waku Pajero | Not working |
| `soniccar` |  | 1991 | Waku Waku Sonic Patrol Car | Playable<br>~60 fps |
| `zunkyou` |  | 1994 | Zunzunkyou no Yabou (Japan) | Playable<br>~60 fps |

</details>

<details>
<summary><b>System C</b> (10 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `bloxeedu` | `bloxeed` | 1989 | Bloxeed (US, C System, Rev A) | Playable<br>~60 fps |
| `bloxeedc` | `bloxeed` | 1989 | Bloxeed (World, C System) | Playable<br>~60 fps |
| `columnsj` | `columns` | 1990 | Columns (Japan) | Playable<br>~60 fps |
| `columnsu` | `columns` | 1990 | Columns (US, cocktail, Rev A) | Playable<br>~60 fps |
| `columns` |  | 1990 | Columns (World) | Playable<br>~60 fps |
| `column2j` | `columns2` | 1990 | Columns II: The Voyage Through Time (Japan) | Playable<br>~60 fps |
| `columns2` |  | 1990 | Columns II: The Voyage Through Time (World) | Playable<br>~60 fps |
| `ichirjbl` | `ichir` | 1994 | Puzzle & Action: Ichidant-R (Japan) (bootleg) | Playable<br>~60 fps |
| `tantrbl2` | `tantr` | 1994 | Puzzle & Action: Tant-R (Japan) (bootleg set 2) | Playable<br>~60 fps |
| `tantrbl3` | `tantr` | 1994 | Puzzle & Action: Tant-R (Japan) (bootleg set 3) | Playable<br>~60 fps |

</details>

## 4. Download

Prebuilt binaries are published on the [Releases](../../releases) page:

| File | Platform | Contents |
|---|---|---|
| `ReST-V_vX.Y.Z_vita.zip` | PS Vita / PS TV | `ReST-V_vX.Y.Z_vita.vpk` and an empty `data/restv/roms/` folder |

The releases contain the emulator only. **No ROMs, BIOS files or game artwork are included or provided.**
You must supply ROM images of games you own.

## 5. Installation

### PS Vita

Requirements:
- A PS Vita or PS TV that can install homebrew VPK files with [VitaShell](https://github.com/TheOfficialFloW/VitaShell).
- The runtime shader compiler `ur0:data/external/libshacccg.suprx`. ReST-V compiles its GPU shaders on the
  device, and games cannot be drawn without it. If it is missing, extract it with
  [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D) or copy it from another homebrew setup guide.

**Install the VPK**

1. Extract `ReST-V_vX.Y.Z_vita.zip` on your PC.
2. Start VitaShell on the Vita and connect it to the PC:
   - USB: press **SELECT** in VitaShell, connect the cable, and the memory card appears as a drive; or
   - FTP: press **SELECT** in VitaShell and connect an FTP client to the address shown on screen.
3. Copy `ReST-V_vX.Y.Z_vita.vpk` to `ux0:` (any folder, for example `ux0:downloads/`).
4. Disconnect, open the folder in VitaShell, select the `.vpk` and press **X** to install.
   Accept the extended permissions prompt. A **ReST-V** bubble appears on the home screen (title ID `RSTV00001`).
5. The `.vpk` file can be deleted after installation.

**Add ROMs**

1. Copy the `data` folder from the zip to the root of `ux0:` so that the empty folder is
   `ux0:data/restv/roms/` (ReST-V also creates it on first launch).
2. Put your ROM zips in that folder, unchanged and not extracted, for example
   `ux0:data/restv/roms/sandor.zip`. For ST-V games also add `stvbios.zip`.
3. Start ReST-V. The game list shows every known set, grouped by board.

The first start of a game compiles the shaders it needs, which can cause a few short pauses. The compiled
shaders are kept in `ux0:data/restv/shaders/`, and later starts load them up front.

### ROM sets

- ROMs must be **MAME** compatible sets with the set names listed in [section 3](#3-supported-boards-and-games).
- Keep each set as one zip named after the set, for example `thuntk.zip`. Do not rename or extract them.
- Clone sets (for example `thuntk`) read missing files from the parent zip (`sandor.zip`), so keep the
  parent in the same folder, or use non-merged sets.
- ST-V sets need `stvbios.zip` in the same folder.
- The *Playable* list shows only sets whose ROM zip was found; press SELECT in the list to show all supported sets.

### Optional files

| What | Path |
|---|---|
| Border artwork | `ux0:data/restv/artwork/<set>.png` (`.jpg` and `.bmp` also work; the parent set name is tried next) |

Files written by the emulator:

| What | Path |
|---|---|
| Settings and favorites | `ux0:data/restv/settings.cfg`, `ux0:data/restv/favorites.cfg` |
| Per-game settings | `ux0:data/restv/<set>_game.cfg` |
| EEPROM / NVRAM | `ux0:data/restv/nvram/<set>.nv` |
| Save states | `ux0:data/restv/state/<set>_<slot>.yss`, quick save `<set>.yss` |

## 6. Usage

**Game list**

| Action | Buttons |
|---|---|
| Start the selected game | Cross (or Circle, see Controls) |
| Change board tab | L / R |
| Page up / down | Left / Right |
| Add / remove favorite | Square |
| Filter: Playable / All / Favorites | SELECT |
| Options (Display, Controls) | START |
| About | Triangle |

Options opened from the game list edit the global settings.

**In game**

| Action | Buttons |
|---|---|
| Lever | D-pad (or left stick, see Controls) |
| Buttons 1-4 | Cross, Circle, Square, Triangle (default) |
| Coin / Start | SELECT / START (default) |
| Open the in-game menu | SELECT + START |
| Quick save / quick load | SELECT + L / SELECT + R |
| Exit | L + R + START |

The in-game menu provides: Resume, Reset, Save states (10 slots), Display (frame rate counter, aspect ratio,
filter, scanlines, border), Controls (button mapping, turbo, analog stick), Cheats (games that have built-in cheats),
per-game settings and game selection.

**Turbo.** Pick one *Turbo key* and the game button it fires (*Turbo button*), plus the speed
(15, 10 or 7.5 presses per second). While the turbo key is held, that game button fires repeatedly.

**Cheats.** Built-in cheats are listed only for the games that have them. *Start stage* (Treasure Hunt, `thunt` and
`thuntk`) starts a new game at stage 2, stage 3 or the final stage; it applies from the next game start, so pick it
and then use *Reset game*. Cheat choices are not saved.

**Per-game settings.** *Save game settings* stores the current display and control settings for the running
set only (`<set>_game.cfg`); global settings are not touched. When a per-game file exists it is loaded
automatically, and the menu shows *Delete game settings* instead.

The last played game is remembered and selected on the next launch.

## 7. Building from Source

Builds are done on Linux or WSL (Ubuntu 24.04 is used for development).

### Prerequisites

```bash
sudo apt install -y build-essential cmake ninja-build git python3 zlib1g-dev
```

- PS Vita: [VitaSDK](https://vitasdk.org/) with `vitashark`, `libpng`, `libjpeg-turbo` and `zlib` (`vdpm` install-all), `VITASDK` set.

### PS Vita

```bash
bash tools/build.sh vita
# output: build-vita/restv.vpk, build-vita/eboot.bin
```

### Desktop test harness

A headless harness runs the ST-V core on a PC and prints per-frame video and audio hashes (`c2harness` does the
same for System C / C2). It is used as the reference for every optimization.

```bash
bash tools/build.sh pc
./build-pc/harness --roms path/to/roms --game sandor --frames 3600 --checkpoint 600
```

An ARM build of the harness (`tools/build_arm_harness.sh`, run under `qemu-arm`) exercises the SH-2
recompiler and the HLE routines with the same hashes.

## 8. Project Layout

```
src/core/      Yabause / YabaSanshiro subset: SH-2, SCU, VDP1/VDP2, SCSP, 68000, SMPC + ST-V I/O, EEPROM, ROM loader
src/c2/        Sega System C / C2 core: 315-5313 VDP, YM3438 (ymfm), SN76489, uPD7759, protection
vita/          PS Vita port: frontend, GPU renderer (sceGxm), platform code, LiveArea assets
tools/         Build scripts, desktop harness, game table generators, hash and audio checks
```

## 9. Accuracy and Testing

- Every performance change is checked against the desktop harness: frame and audio hashes are compared
  before and after over thousands of frames on a set of reference games.
- Changes that alter the hashes on purpose (idle-loop skipping, relaxed CPU synchronization, HLE routines)
  are checked with save states and recorded input against reference screenshots, so that the game reaches
  the same scenes with the same picture and sound.
- The recompiler and HLE routines are compared against the interpreter in the ARM harness.

## 10. License and Credits

**License.** ReST-V is distributed under the [GNU General Public License, version 3](LICENSE).
- The Yabause / YabaSanshiro / Kronos code and the ReST-V code are GPL-2.0-or-later.
- The MAME parts and ymfm are BSD-3-Clause, Musashi is MIT, JUnzip is public domain.
- vitaShaRK, linked into the Vita build, is LGPL-3.0, so the combined program and the release packages
  are distributed under GPL-3.0.

The original copyright headers are kept in all imported sources.

**Credits**
- [Yabause](https://github.com/libretro/yabause) team and devMiyax ([YabaSanshiro](https://github.com/devmiyax/yabause)):
  SH-2, SCU, VDP1/VDP2, SCSP, SMPC and the SH-2 recompiler.
- [Kronos](https://github.com/FCare/Kronos) (FCare): ST-V I/O, EEPROM and cartridge support.
- [MAME](https://www.mamedev.org/) developers: ST-V and System C2 ROM definitions, 315-5881 decryption,
  System C2 protection and hardware research.
- [Musashi](https://github.com/kstenerud/Musashi) (Karl Stenerud): 68000 core.
- [ymfm](https://github.com/aaronsgiles/ymfm) (Aaron Giles): YM3438.
- [VitaSDK](https://vitasdk.org/), [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) (Rinnegatamante).
- [JUnzip](https://github.com/jokkebk/JUnzip), [zlib](https://zlib.net/), [libpng](https://github.com/pnggroup/libpng),
  [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo).

**Trademarks.** Sega, ST-V and all game titles are trademarks of their respective owners. This project is not
affiliated with or endorsed by Sega or any rights holder. No ROM images, BIOS files, game graphics or logos are
included in this repository or in the release packages.

## 11. Support

ReST-V is free software. If you want to support its development, you can do so on
[Patreon](https://www.patreon.com/Reyanon). Support is for the emulator work only.

**ROMs and BIOS files are never provided, to anyone, including supporters.** Do not ask for them.
