#include "emu.h"
#include <raylib.h>

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

// Windowed frontend: run at ~60fps and blit the framebuffer scaled 3x.
static void run_window(Bus *bus, CPU *cpu) {
  InitWindow(256 * 3, 240 * 3, "nes-emu");
  SetTargetFPS(60);
  unsigned char *buf = (unsigned char *)malloc(256 * 240 * 4);
  Image img;
  img.data = buf;
  img.width = 256;
  img.height = 240;
  img.mipmaps = 1;
  img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
  Texture2D tex = LoadTextureFromImage(img);
  while (!WindowShouldClose()) {
    u8 pad0 = 0;
    if (IsKeyDown(KEY_X))
      pad0 |= 0x01; // A
    if (IsKeyDown(KEY_Z))
      pad0 |= 0x02; // B
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
      pad0 |= 0x04; // Select
    if (IsKeyDown(KEY_ENTER))
      pad0 |= 0x08; // Start
    if (IsKeyDown(KEY_UP))
      pad0 |= 0x10;
    if (IsKeyDown(KEY_DOWN))
      pad0 |= 0x20;
    if (IsKeyDown(KEY_LEFT))
      pad0 |= 0x40;
    if (IsKeyDown(KEY_RIGHT))
      pad0 |= 0x80;
    bus->joypad.pad[0] = pad0;
    run_frame(bus, cpu);
    for (i32 i = 0; i < 256 * 240; ++i) {
      buf[i * 4 + 0] = bus->ppu.pixels[i / 256][i % 256][0];
      buf[i * 4 + 1] = bus->ppu.pixels[i / 256][i % 256][1];
      buf[i * 4 + 2] = bus->ppu.pixels[i / 256][i % 256][2];
      buf[i * 4 + 3] = 255;
    }
    UpdateTexture(tex, buf);
    BeginDrawing();
    ClearBackground(BLACK);
    DrawTextureEx(tex, (Vector2){0, 0}, 0, 3.0f, WHITE);
    EndDrawing();
  }
  UnloadTexture(tex);
  free(buf);
  CloseWindow();
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

i32 main(int argc, char **argv) {
  const char *rom_file = NULL;
  const char *dump_file = NULL;
  u32 frames = 0;

  for (i32 i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-g") == 0 && i + 1 < argc)
      rom_file = argv[++i];
    else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
      frames = (u32)strtoul(argv[++i], NULL, 10);
    else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
      dump_file = argv[++i];
    else {
      fprintf(stderr, "Usage: %s -g <game.nes> [-f frames] [--dump out.ppm]\n",
              argv[0]);
      return 1;
    }
  }

  if (!rom_file) {
    fprintf(stderr, "Usage: %s -g <game.nes> [-f frames] [--dump out.ppm]\n",
            argv[0]);
    return 1;
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

  // Self-check: $4016/$4017 shift independently; a game interleaving 8
  {
    Bus t;
    init_bus(&t);
    t.joypad.pad[0] = 0x5A;
    t.joypad.pad[1] = 0x33;
    bus_write(&t, 0x4016, 1);
    bus_write(&t, 0x4016, 0);
    u8 p1 = 0, p2 = 0;
    for (i32 i = 0; i < 8; ++i) {
      p1 = (u8)((p1 >> 1) | ((bus_read(&t, 0x4016) & 1) << 7));
      p2 = (u8)((p2 >> 1) | ((bus_read(&t, 0x4017) & 1) << 7));
    }
    if (p1 != 0x5A || p2 != 0x33) {
      fprintf(stderr, "joypad self-check FAIL p1=%02X p2=%02X\n", p1, p2);
      return 1;
    }
  }

  CPU cpu;
  cpu_reset(&cpu, &bus);

  if (frames == 0) {
    run_window(&bus, &cpu);
    return 0;
  }

  for (u32 i = 0; i < frames; ++i)
    run_frame(&bus, &cpu);

  if (dump_file) {
    if (!dump_ppm(&bus, dump_file)) {
      fprintf(stderr, "Failed to write frame dump\n");
      return 1;
    }
  }

  return 0;
}
