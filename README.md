# NES-emu

A NES emulator in C, grown out of a MOS 6502 CPU emulator
([emu-6502](https://github.com/KirolosBoshra/emu-6502)).

## Build

```
cmake -B build
cmake --build build
```

## Run

```
nes-emu.exe <game.nes> [frames] [--dump out.ppm]
```

Runs `<game>.nes` until `frames` frames are rendered (default 3); `--dump out.ppm` writes the final frame as a PPM image for headless inspection.

## Status

Implemented: full 6502 core (all 151 official opcodes, IRQ/NMI, cycle timing), iNES cartridge loader + NROM (mapper 0), PPU (registers, scroll/VRAM addressing, `$4014` OAM DMA, palette RAM, background + sprite rendering, vblank NMI), headless frame dump.

Not yet: raylib window, controllers, APU/audio, mappers 2/4 (UxROM/MMC3). ROMs requiring unsupported mappers are rejected with a clear error.

## Screenshots

![Pac-Man](imgs/pacman.png)

![Tic-Tac-Toe](imgs/tictacxo.png)

## Dev

Throw a `.nes` ROM at it:

```sh
cmake --build build
./build/nes-emu.exe path/to/game.nes 30 --dump frame.ppm
```