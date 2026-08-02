#ifndef NES_H
#define NES_H

#include "types.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define PRG_BANK 0x4000 // 16KB
#define CHR_BANK 0x2000 // 8KB

typedef struct {
  u8 prg[32][PRG_BANK];
  u8 chr[64][CHR_BANK];

  usize prg_banks, chr_banks; // How many banks
  u8 mapper;
  bool vertical_mirror;
} Cartridge;

typedef struct {
  u8 ctrl, mask, status;  // $2000/2001/2002
  u8 oam_addr;            // $2003
  u8 oam[256];            // sprite RAM
  u8 vram[0x1000];        // 4 nametables (4 x 1KB); palette kept in pal[]
  u16 vaddr;              // $2006 fallback bus / current VRAM address
  u16 t;                  // scroll+nametable register ($2005/$2006 combo)
  u8 fine_x;              // fine X scroll (0-7)
  bool write_toggle;      // $2005/$2005 and $2006/$2007 latch
  u8 read_buffer;         // $2007 read latch (returned_T) for open bus
  bool in_vblank;         // true if in vblank
  u8 pal[0x20];           // 64B palette RAM ($3F00-$3F1F)
  u8 pixels[240][256][3]; // rendered frame (RGB888)
  u64 dots;               // PPU dots since last frame (NTSC: 89342/frame)
  bool frame_done;        // raised at end of frame
} PPU;

static bool cart_load_file(Cartridge *cart, const char *filename) {
  memset(cart, 0, sizeof(*cart));
  FILE *file = fopen(filename, "rb");
  if (!file)
    return false;

  u8 header[16];
  if (fread(header, 1, 16, file) != 16 || memcmp(header, "NES\x1A", 4)) {
    fclose(file);
    return false;
  }

  cart->prg_banks = header[4];
  cart->chr_banks = header[5];
  if (cart->prg_banks > 32 || cart->chr_banks > 64) {
    fclose(file);
    return false;
  }
  cart->mapper = (header[6] >> 4) | (header[7] & 0xF0);
  cart->vertical_mirror = header[6] & 1;

  fseek(file, (header[6] & 0x04) ? 512 : 0, SEEK_CUR); // skip trainer

  // actually fread(cart->prg, ...)
  fread(cart->prg, PRG_BANK, cart->prg_banks, file);
  fread(cart->chr, CHR_BANK, cart->chr_banks, file);
  fclose(file);

  if (cart->prg_banks == 0)
    return false;
  return true;
}

static u8 cart_read_prg(Cartridge *cart, u16 address) {
  if (address < 0x8000 || cart->prg_banks == 0)
    return 0;

  u32 offset = (u32)(address - 0x8000); // PRG window starts at $8000
  u32 bank = (offset / PRG_BANK) %
             cart->prg_banks; // 16KB cart: 0,0 → mirror; 32KB: 0,1

  return cart->prg[bank][offset % PRG_BANK];
}

// PPU (2C02)

// Standard NTSC NES palette (2C02) as decoded to sRGB.
static const u8 ppu_palette[64][3] = {
    {124, 124, 124}, {0, 0, 252},     {0, 0, 188},     {68, 40, 188},
    {148, 0, 132},   {168, 0, 32},    {168, 16, 0},    {136, 20, 0},
    {80, 48, 0},     {0, 120, 0},     {0, 104, 0},     {0, 88, 0},
    {0, 64, 88},     {0, 0, 0},       {0, 0, 0},       {0, 0, 0},
    {188, 188, 188}, {0, 120, 248},   {0, 88, 248},    {104, 68, 252},
    {216, 0, 204},   {228, 0, 88},    {248, 56, 0},    {228, 92, 16},
    {172, 124, 0},   {0, 184, 0},     {0, 168, 0},     {0, 168, 68},
    {0, 136, 136},   {0, 0, 0},       {0, 0, 0},       {0, 0, 0},
    {248, 248, 248}, {60, 188, 252},  {104, 136, 252}, {152, 120, 248},
    {248, 120, 248}, {248, 88, 152},  {248, 120, 88},  {252, 160, 68},
    {248, 184, 0},   {184, 248, 24},  {88, 216, 84},   {88, 248, 152},
    {0, 232, 216},   {120, 120, 120}, {0, 0, 0},       {0, 0, 0},
    {252, 252, 252}, {164, 228, 252}, {184, 184, 248}, {216, 184, 248},
    {248, 184, 248}, {248, 164, 192}, {240, 208, 176}, {252, 224, 168},
    {248, 216, 120}, {216, 248, 120}, {184, 248, 184}, {184, 248, 216},
    {0, 252, 252},   {248, 216, 248}, {0, 0, 0},       {0, 0, 0},
};

// Map a $3F00-$3FFF address (lo 6 bits) to its real palette slot.
static u8 ppu_pal_index(u16 addr) {
  u8 i = addr & 0x1F;
  if ((i & 0x03) == 0) // $3F00/10/04/14/etc mirror
    i &= 0x0F;
  return i;
}

// Mirrors nametable index (0-3) per the cartridge's mirroring flag.
static u8 ppu_nt_index(const Cartridge *cart, u8 i) {
  if (cart->vertical_mirror)
    return i & 1;         // A B A B
  return (i & 2) ? 2 : 0; // A A B B
}

// Renders the current frame into pixels.
static void ppu_render(PPU *ppu, const Cartridge *cart) {
  // Clear to the universal background color.
  u8 bg = ppu->pal[0];
  for (int y = 0; y < 240; ++y)
    for (int x = 0; x < 256; ++x)
      memcpy(ppu->pixels[y][x], ppu_palette[bg], 3);

  if (cart->chr_banks == 0)
    return;

  static const u8 plane_mask[2] = {0x08, 0x10};
  bool show_bg = ppu->mask & plane_mask[0];
  bool show_sp = ppu->mask & plane_mask[1];

  // Scroll origin in pixels (coarse*8 + fine).
  int base_x = (ppu->t & 0x1F) * 8 + ppu->fine_x;
  int base_y = ((ppu->t >> 5) & 0x1F) * 8 + ((ppu->t >> 12) & 7);

  // Background: two-plane 8x8 tiles, wrapped across the 4 nametables.
  if (show_bg) {
    for (int sy = 0; sy < 240; ++sy) {
      int gy = base_y + sy; // wrap into coarse row
      int tyrow = (gy >> 3) & 0x1F;
      int nt_y = gy >> 8; // 0..1 vertical wrap
      for (int sx = 0; sx < 256; ++sx) {
        int gx = base_x + sx;
        int txcol = (gx >> 3) & 0x1F;
        int nt_x = gx >> 8; // 0..1 horizontal wrap
        u8 nt = ppu_nt_index(cart, nt_y * 2 + nt_x) * 0x400;
        u32 tile = ppu->vram[nt + tyrow * 32 + txcol];
        u32 attr = ppu->vram[nt + 0x3C0 + (tyrow / 4) * 8 + (txcol / 4)];
        u32 palt = ((attr >> (((tyrow & 2) << 1) | (txcol & 2))) & 3) << 2;
        const u8 *pat = cart->chr[(ppu->ctrl >> 4) & 1] + tile * 16;
        u32 row = gy & 7, col = gx & 7;
        u8 lo = pat[row], hi = pat[row + 8];
        u32 pix = ((lo >> (7 - col)) & 1) | (((hi >> (7 - col)) & 1) << 1);
        u8 idx = ppu->pal[palt + pix]; // pix==0 -> backdrop (pal[0])
        memcpy(ppu->pixels[sy][sx], ppu_palette[idx], 3);
      }
    }
  }

  // Sprites: 8x8, over background, no flips yet.
  if (show_sp) {
    for (int k = 0; k < 64; ++k) {
      u8 oy = ppu->oam[k * 4 + 0];
      u8 tile = ppu->oam[k * 4 + 1];
      u8 at = ppu->oam[k * 4 + 2];
      u8 ox = ppu->oam[k * 4 + 3];
      if (oy == 0 || oy >= 240)
        continue;
      u8 palt2 = 0x10 + ((at & 3) << 2);
      const u8 *pat = cart->chr[(ppu->ctrl >> 4) & 1] + tile * 16;
      for (int l = 0; l < 8; ++l)
        for (int t = 0; t < 8; ++t) {
          int px = ox + t - 1; // sprite fetch starts one pixel earlier
          int py = oy + l - 1;
          if (py < 0 || py >= 240 || px < 0 || px >= 256)
            continue;
          u8 lo = pat[l], hi = pat[l + 8];
          u8 pix = ((lo >> (7 - t)) & 1) | (((hi >> (7 - t)) & 1) << 1);
          if (pix) {
            u8 ci = ppu->pal[palt2 + pix];
            memcpy(ppu->pixels[py][px], ppu_palette[ci], 3);
          }
        }
    }
  }
}

// Tick PPU by `dt` dots (bits of CPU) and drive NMI@vblank.
static bool ppu_tick(PPU *ppu, u64 dt) {
  bool nmi = false;
  ppu->dots += dt;
  if (!ppu->in_vblank && ppu->dots >= 82181) { // scanline 241
    ppu->in_vblank = true;
    ppu->status |= 0x80;
    if (ppu->ctrl & 0x80)
      nmi = true;
  }
  if (ppu->dots >= 89001) { // scanline 261 -> clear vblank flag
    ppu->status &= ~0x80;
  }
  if (ppu->dots >= 89342) {
    ppu->dots -= 89342;
    ppu->in_vblank = false;
    ppu->frame_done = true;
  }
  return nmi;
}

static u8 ppu_read_reg(PPU *ppu, u8 reg) {
  switch (reg & 7) {
  case 2: { // $2002: status, clears vblank-on-read
    u8 v = ppu->status;
    ppu->status &= ~0x80;
    ppu->write_toggle = false;
    return v;
  }
  case 4: { // $2004: OAM read
    u8 v = ppu->oam[ppu->oam_addr];
    if ((ppu->oam_addr & 3) == 2)
      v &= 0xE3; // nd: bits 1/5..7 fixed
    ppu->oam_addr = (ppu->oam_addr + 1) & 0xFF;
    return v;
  }
  case 7: { // $2007: buffered read
    u16 a = ppu->vaddr;
    if (a >= 0x3F00) {
      u8 i = ppu_pal_index((u8)a);
      u8 out = ppu->pal[i];
      ppu->read_buffer = ppu->vram[a & 0xFFF]; // open-bus poke
      ppu->vaddr = (ppu->vaddr + 1) & 0x7FFF;
      return out;
    } else {
      u8 out = ppu->read_buffer;
      ppu->read_buffer = ppu->vram[a & 0xFFF];
      ppu->vaddr = (ppu->vaddr + 1) & 0x7FFF;
      return out;
    }
  }
  default:
    return 0; // read-only regs return 0 for simplicity
  }
}

static void ppu_write_reg(PPU *ppu, u8 reg, u8 value) {
  switch (reg & 7) {
  case 0:
    ppu->ctrl = value;
    break;
  case 1:
    ppu->mask = value & 0x1F;
    break; // game only cares low bits
  case 3:
    ppu->oam_addr = value;
    break;
  case 4:
    ppu->oam[ppu->oam_addr] = value;
    ppu->oam_addr = (ppu->oam_addr + 1) & 0xFF;
    break;
  case 5: { // scroll (two writes)
    if (!ppu->write_toggle)
      ppu->t = (ppu->t & 0xFFE0) | (value >> 3), ppu->fine_x = value & 7;
    else
      ppu->t = (ppu->t & 0xBFFF) | ((value & 7) << 12) | ((value & 0xF8) << 2);
    ppu->write_toggle ^= 1;
    break;
  }
  case 6: { // vram address (two writes)
    if (!ppu->write_toggle)
      ppu->t = (ppu->t & 0x00FF) | ((value & 0x3F) << 8);
    else
      ppu->t = (ppu->t & 0xFF00) | value;
    ppu->vaddr = ppu->t;
    ppu->write_toggle ^= 1;
    break;
  }
  case 7: {
    if (ppu->vaddr >= 0x3F00)
      ppu->pal[ppu_pal_index((u8)ppu->vaddr)] = value;
    else
      ppu->vram[ppu->vaddr & 0x0FFF] = value;
    ppu->vaddr = (ppu->vaddr + 1) & 0x7FFF;
    break;
  }
  }
}

#endif
