# Weapon ranges

Data: `lua.dat` and `lua.fat` on both discs. Found 2026-10-25 from the compiled weapon scripts, read with
tools/luadat.py; changed with tools/weapon_range.py.

## The archives

`lua.fat` indexes `lua.dat` (and likewise `common`, `weaponpack`, the sound banks): `DAT2`, a count, then
per file its offset, size and a hash of its name (12 bytes; the names are not stored). All 404 files of
Instincts' `lua.dat` and 448 of Evolution's are compiled Lua scripts.

## The scripts

A Lua 5.0-like chunk: the header `1B 'Lua' 41`, then little endian, 4-byte ints, size_t and instructions,
and 4-byte float numbers (the test number is 314159265 as a float). Each function: source (`=(none)`),
five ints, line info (empty), locals (empty), typed constants (3 a string, 0 nil, else a float), nested
functions, code. The reader ends exactly at the end of every script in both games.

An instruction is 32 bits: op = w & 0x3F, A = w >> 24, Bx = (w >> 6) & 0x3FFFF, sBx = Bx - 0x1FFFF.

| Op | | |
|---|---|---|
| 1 | LOADK | R(A) = K(Bx) |
| 2 | LOADINT | R(A) = sBx: integers are not constants |
| 8 | SETTABLE | R((w >> 15) & 0x1FF)[RK((w >> 6) & 0x1FF)] = R(A); RK from 250 up is constant RK - 250 |

So `damage = 30` in a table compiles to a LOADINT and a SETTABLE whose key is the constant `damage`.

## FireParams ranges

Each weapon script (its first string constant is the weapon) builds a `FireParams` table with
`distance` and `distance_max` in metres, and the same for multiplayer. Played: a shot past
`distance_max` does not hit, which is why the guns seem to stop short. Read in both games, identical
in both (Evolution adds the blowgun):

| Weapon | distance | distance_max | multiplayer |
|---|---|---|---|
| Berretta, BerrettaNoSup (Beretta M9) | 25 | 30 | 20 / 35 |
| Glock | 20 | 30 | 15 / 30 |
| DE (Desert Eagle) | 30 | 40 | 30 / 45 |
| MP5, P90 | 30 | 40 | 20 / 35 |
| M16 | 30 | 45 | 35 / 45 |
| M4 | 35 | 70 | 40 / 55 |
| FNM249 | 35 | 45 | 30 / 40 |
| Browning | 40 | 60 | 35 / 60 (Evolution 35 / 70) |
| HKSL8, BarrettM82A1 | 150 | 200 | 200 / 250 |
| M1S90 (shotgun) | 10 | 20 | 10 / 20 (Evolution 15 / 25) |
| M202 | 100 | 110 | 100 / 110 |
| BlowGun (Evolution) | 15 | 20 | 10 / 15 |
| DragonDart | 30 | 30 | |
| Seringe_Gun | 1.6 | 3.2 (constants) | |

The M16 script has a second table of zeros (its grenade launcher); melee weapons and grenades are 0.

## AI ranges

The character scripts set `sightrange` (mostly 150 to 220 m for soldiers), `attackrange` (70 m in 17
of 22; read in Instincts) and `commrange`. Enemies fire from their weapon's script too, but only within `attackrange`.
With longer weapon ranges the player can hit enemies from farther than they return fire.

## Longer ranges (tools/weapon_range.py)

The tool rewrites each range's LOADINT in place (`RANGES`: pistols 50 / 150, the MP5 and P90 100 / 250,
rifles and the M249 250 / 600, the Browning 300 / 800, the SL8 400 / 1000, the Barrett 600 / 1500, the
shotgun 25 / 60). `lua.dat` keeps its size, so `lua.fat` stays valid, and no other byte changes (checked
against the original in both games: 26 instructions, nothing else). The original stays as
`lua.dat.orig`. Multiplayer ranges, `attackrange` and the rest are left.

Played by hand: shots land well past the old limits, and enemies hold fire until their attack range.

Open: where the engine reads `distance` and `distance_max`, and how the damage falls between them
(full damage up to `distance` is read from the name).
