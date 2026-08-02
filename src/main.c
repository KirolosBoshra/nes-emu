#include "emu.h"

// Run the CPU/PPU until one rendered frame completes.
static void run_frame(Bus *bus, CPU *cpu) {
  bus->ppu.frame_done = false;
  u64 last = cpu->cycles;
  while (!bus->ppu.frame_done) {
    cpu_step(cpu, bus);
    bool nmi = ppu_tick(&bus->ppu, (cpu->cycles - last) * 3);
    last = cpu->cycles;
    if (nmi)
      bus->nmi_pending = true;
  }
  ppu_render(&bus->ppu, &bus->cart);
}

static bool dump_ppm(Bus *bus, const char *filename) {
  FILE *file = fopen(filename, "wb");
  if (!file)
    return false;
  fprintf(file, "P6\n256 240\n255\n");
  fwrite(&bus->ppu.pixels[0][0][0], 1, 256 * 240 * 3, file);
  fclose(file);
  return true;
}

int main(int argc, char **argv) {
  const char *rom_file = argv[1];
  const char *dump_file = NULL;
  u32 frames = 3;

  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
      dump_file = argv[++i];
    else if (argv[i][0] != '-')
      frames = (u32)strtoul(argv[i], NULL, 10);
    else {
      fprintf(stderr, "Usage: %s <game.nes> [frames] [--dump out.ppm]\n",
              argv[0]);
      return 1;
    }
  }

  static Bus bus;
  init_bus(&bus);

  if (!cart_load_file(&bus.cart, rom_file)) {
    fprintf(stderr, "Failed to load cartridge: %s\n", rom_file);
    return 1;
  }

  if (bus.cart.mapper != 0) {
    fprintf(stderr, "Unsupported mapper %u (only NROM/0 supported)\n",
            (unsigned)bus.cart.mapper);
    return 1;
  }

  // Self-check: PRG mapping via bus matches the cartridge bytes across the
  // full window, including the 16KB-pair mirror.
  for (u32 addr = 0x8000; addr < 0x10000; ++addr) {
    u32 offset = addr - 0x8000;
    u32 bank = (offset / PRG_BANK) % bus.cart.prg_banks;
    if (bus_read(&bus, (u16)addr) != bus.cart.prg[bank][offset % PRG_BANK]) {
      fprintf(stderr, "MISMATCH addr=%04X\n", (unsigned)addr);
      return 1;
    }
  }

  CPU cpu;
  cpu_reset(&cpu, &bus);

  printf("PRG banks: %u  CHR banks: %u\n", (unsigned)bus.cart.prg_banks,
         (unsigned)bus.cart.chr_banks);
  printf("Reset vector: 0x%04X\n", (unsigned)cpu.PC);

  for (u32 i = 0; i < frames; ++i) {
    run_frame(&bus, &cpu);
    printf("frame %u  PC=0x%04X  cycles=%llu\n", i, (unsigned)cpu.PC,
           (unsigned long long)cpu.cycles);
  }
  printf("ppu dots=%llu status=0x%02X\n",
         (unsigned long long)bus.ppu.dots, (unsigned)bus.ppu.status);

  if (dump_file) {
    fprintf(stderr, "t=%04X vaddr=%04X ctrl=%02X mask=%02X pal0=%02X\n",
            (unsigned)bus.ppu.t, (unsigned)bus.ppu.vaddr, (unsigned)bus.ppu.ctrl,
            (unsigned)bus.ppu.mask, (unsigned)bus.ppu.pal[0]);
    int n = 0;
    for (int i = 0; i < 0x400; i++)
      if (bus.ppu.vram[i])
        n++;
    fprintf(stderr, "vram[0..0x3FF] nonzero=%d\n", n);
    if (!dump_ppm(&bus, dump_file)) {
      fprintf(stderr, "Failed to write frame dump\n");
      return 1;
    }
    printf("wrote %s\n", dump_file);
  }

  return 0;
}