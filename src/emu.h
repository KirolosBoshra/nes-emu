#ifndef EMU_H
#define EMU_H

#include "nes.h"
#include "types.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Type Definitions

typedef struct {
  u8 A, X, Y, SP; // Accumulator, Index Registers, Stack Pointer
  u16 PC;         // Program Counter
  u8 P;           // Processor Status
  u64 cycles;     // Cycle Count
} CPU;

typedef enum {
  FLAG_C = 1u << 0,
  FLAG_Z = 1u << 1,
  FLAG_I = 1u << 2,
  FLAG_D = 1u << 3,
  FLAG_B = 1u << 4,
  FLAG_U = 1u << 5,
  FLAG_V = 1u << 6,
  FLAG_N = 1u << 7,
} CPUFlag;

typedef struct {
  u8 pad[2];        // button snapshot (bit0=A,1=B,2=Select,3=Start,4=Up,5=Down,6=Left,7=Right)
  u8 pad_shift[2];  // shift register snapshot
  u8 pad_index[2];  // read position per pad ($4016/$4017 are separate registers)
  bool pad_strobe;
} Joypad;

typedef struct {
  u8 ram[0x800]; // 2KB internal RAM

  Cartridge cart;
  PPU ppu;
  Joypad joypad;

  bool nmi_pending;
  bool irq_pending;

  u8 (*read)(void *self, u16 addr);
  void (*write)(void *self, u16 addr, u8 value);

  void *self; // Pointer to it self
} Bus;

// Opcode Handler Trait
typedef void (*OpcodeHandler)(CPU *cpu, Bus *bus, u8 opcode);

// Function Declarations

static void cpu_reset(CPU *cpu, Bus *bus) {
  cpu->A = 0;
  cpu->X = 0;
  cpu->Y = 0;
  cpu->SP = 0xFD; // Stack Pointer starts at 0xFD
  cpu->PC = bus->read(bus->self, 0xFFFC) |
            (bus->read(bus->self, 0xFFFD) << 8); // Reset Vector
  cpu->P = FLAG_I | FLAG_U; // Unused flag and Interrupt Disable flag set
  cpu->cycles = 0;
}

static void cpu_set_flag(CPU *cpu, CPUFlag flag, bool condition) {
  if (condition)
    cpu->P |= flag;
  else
    cpu->P &= ~flag;
}

static void cpu_update_zero_and_negative_flags(CPU *cpu, u8 value) {
  cpu_set_flag(cpu, FLAG_Z, value == 0);
  cpu_set_flag(cpu, FLAG_N, (value & 0x80) != 0);
}

static u8 bus_read(void *self, u16 addr) {
  Bus *bus = (Bus *)self;
  if (addr < 0x2000)
    return bus->ram[addr & 0x7FF];
  if (addr < 0x4000)
    return ppu_read_reg(&bus->ppu, addr & 7);
  if (addr == 0x4016 || addr == 0x4017) {
    // Controller read: strobe high -> A held, else shift out one bit.
    // ponytail: index wraps at 8 instead of emitting open-bus 1s; games
    // only read 8 bits. $4016/$4017 shift independently (games interleave
    // the reads), so each pad keeps its own index.
    u8 p = addr & 1;
    if (bus->joypad.pad_strobe)
      return bus->joypad.pad[p] & 1;
    u8 bit = (bus->joypad.pad_shift[p] >> bus->joypad.pad_index[p]) & 1;
    bus->joypad.pad_index[p] = (bus->joypad.pad_index[p] + 1) & 7;
    return bit;
  }
  // $4000+ -> APU registers (stubs) or PRG ROM
  return cart_read_prg(&bus->cart, addr);
}

static void ppu_oam_dma(PPU *ppu, Bus *bus, u8 page);

static void bus_write(void *self, u16 addr, u8 value) {
  Bus *bus = (Bus *)self;
  if (addr < 0x2000) {
    bus->ram[addr & 0x7FF] = value;
    return;
  }
  if (addr < 0x4000) {
    ppu_write_reg(&bus->ppu, addr & 7, value);
    return;
  }
  if (addr == 0x4014)
    ppu_oam_dma(&bus->ppu, bus, value);
  else if (addr == 0x4016) { // strobe: latch buttons on 1->0 edge
    bus->joypad.pad_strobe = value & 1;
    if (!(value & 1)) {
      bus->joypad.pad_shift[0] = bus->joypad.pad[0];
      bus->joypad.pad_shift[1] = bus->joypad.pad[1];
      bus->joypad.pad_index[0] = 0;
      bus->joypad.pad_index[1] = 0;
    }
  }
  // Other $4000+ and PRG ROM writes ignored (stubs)
}

static void ppu_oam_dma(PPU *ppu, Bus *bus, u8 page) {
  const u8 *src = bus->ram + ((u16)page << 8);
  memcpy(ppu->oam, src, 256);
  ppu->oam_addr = 0;
}

static void init_bus(Bus *bus) {
  memset(bus, 0, sizeof(*bus));
  bus->read = bus_read;
  bus->write = bus_write;
  bus->self = bus;
}

static u8 fetch8(CPU *cpu, Bus *bus) { return bus->read(bus->self, cpu->PC++); }

static u16 fetch16(CPU *cpu, Bus *bus) {
  u16 lo = fetch8(cpu, bus);
  u16 hi = fetch8(cpu, bus);

  // Little Endian
  return lo | (hi << 8);
}

static void cpu_nmi(CPU *cpu, Bus *bus) {
  bus->write(bus->self, 0x0100 + cpu->SP--, (cpu->PC >> 8) & 0xFF);
  bus->write(bus->self, 0x0100 + cpu->SP--, cpu->PC & 0xFF);
  bus->write(bus->self, 0x0100 + cpu->SP--, cpu->P & ~FLAG_B);

  cpu_set_flag(cpu, FLAG_I, true);

  u16 lo = bus->read(bus->self, 0xFFFA);
  u16 hi = bus->read(bus->self, 0xFFFB);
  cpu->PC = lo | (hi << 8);

  cpu->cycles += 7;
  bus->nmi_pending = false;
}

static void cpu_irq(CPU *cpu, Bus *bus) {
  bus->write(bus->self, 0x0100 + cpu->SP--, (cpu->PC >> 8) & 0xFF);
  bus->write(bus->self, 0x0100 + cpu->SP--, cpu->PC & 0xFF);
  bus->write(bus->self, 0x0100 + cpu->SP--, cpu->P & ~FLAG_B);

  cpu_set_flag(cpu, FLAG_I, true);
  u16 lo = bus->read(bus->self, 0xFFFE);
  u16 hi = bus->read(bus->self, 0xFFFF);
  cpu->PC = lo | (hi << 8);

  cpu->cycles += 7;
  bus->irq_pending = false;
}

// Instruction Handlers

static void op_illegal(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;

  fprintf(stderr, "Illegal/unimplemented opcode: 0x%02X at PC=0x%04X\n",
          (unsigned)opcode, (unsigned)(u16)(cpu->PC - 1));

  exit(1);
}

static void op_nop(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;
  (void)opcode;
  cpu->cycles += 2; // NOP takes 2 cycles
}

static void op_lda_imm(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u8 value = fetch8(cpu, bus);
  cpu->A = value;
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += 2; // LDA immediate takes 2 cycles
}

static void op_lda_zp(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u8 addr = fetch8(cpu, bus);
  u8 value = bus->read(bus->self, addr);
  cpu->A = value;
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += 3; // LDA zero page takes 3 cycles
}

static void op_lda_abs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u16 addr = fetch16(cpu, bus);
  u8 value = bus->read(bus->self, addr);
  cpu->A = value;
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += 4;
}

static void op_ldx_imm(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u8 value = fetch8(cpu, bus);
  cpu->X = value;
  cpu_update_zero_and_negative_flags(cpu, cpu->X);
  cpu->cycles += 2; // LDX immediate takes 2 cycles
}

static void op_ldx_zp(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u8 addr = fetch8(cpu, bus);
  u8 value = bus->read(bus->self, addr);
  cpu->X = value;
  cpu_update_zero_and_negative_flags(cpu, cpu->X);
  cpu->cycles += 3; // LDX zero page takes 3 cycles
}

static void op_ldy_imm(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u8 value = fetch8(cpu, bus);
  cpu->Y = value;
  cpu_update_zero_and_negative_flags(cpu, cpu->Y);
  cpu->cycles += 2;
}

static void op_ldy_zp(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u8 addr = fetch8(cpu, bus);
  u8 value = bus->read(bus->self, addr);
  cpu->Y = value;
  cpu_update_zero_and_negative_flags(cpu, cpu->Y);
  cpu->cycles += 3;
}

static void op_sta_zp(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u8 addr = fetch8(cpu, bus);
  bus->write(bus->self, addr, cpu->A);
  cpu->cycles += 3;
}

static void op_sta_abs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u16 addr = fetch16(cpu, bus);

  bus->write(bus->self, addr, cpu->A);

  cpu->cycles += 4; // STA absolute takes 4 cycles
}

static void op_stx_abs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u16 addr = fetch16(cpu, bus);

  bus->write(bus->self, addr, cpu->X);

  cpu->cycles += 4; // STX absolute takes 4 cycles
}

static void op_sty_abs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u16 addr = fetch16(cpu, bus);

  bus->write(bus->self, addr, cpu->Y);

  cpu->cycles += 4; // STX absolute takes 4 cycles
}

static void op_jmp_abs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  cpu->PC = fetch16(cpu, bus);

  cpu->cycles += 3; // JMP absolute takes 3 cycles
}

static void op_jmp_ind(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u16 addr = fetch16(cpu, bus);

  // Emulate the 6502 bug where the high byte does not wrap correctly
  u16 lo = bus->read(bus->self, addr);
  u16 hi = bus->read(bus->self, (addr & 0xFF00) | ((addr + 1) & 0x00FF));

  cpu->PC = lo | (hi << 8);

  cpu->cycles += 5; // JMP indirect takes 5 cycles
}

static void op_brk(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  // Push PC and P onto the stack
  bus->write(bus->self, 0x0100 + cpu->SP--, (cpu->PC >> 8) & 0xFF);
  bus->write(bus->self, 0x0100 + cpu->SP--, cpu->PC & 0xFF);
  bus->write(bus->self, 0x0100 + cpu->SP--, cpu->P | FLAG_B);

  // Set the interrupt disable flag
  cpu_set_flag(cpu, FLAG_I, true);

  // Load the interrupt vector
  u16 lo = bus->read(bus->self, 0xFFFE);
  u16 hi = bus->read(bus->self, 0xFFFF);
  cpu->PC = lo | (hi << 8);

  cpu->cycles += 7; // BRK takes 7 cycles
}

static void op_php(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  // Push P onto the stack
  bus->write(bus->self, 0x0100 + cpu->SP--, cpu->P | FLAG_B);

  cpu->cycles += 3; // PHP takes 3 cycles
}

// Flags
static void op_clc(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  (void)bus;
  cpu_set_flag(cpu, FLAG_C, false);
  cpu->cycles += 2;
}

static void op_cld(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  (void)bus;

  cpu_set_flag(cpu, FLAG_D, false);
  cpu->cycles += 2;
}

static void op_cli(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  (void)bus;

  cpu_set_flag(cpu, FLAG_I, false);
  cpu->cycles += 2;
}

static void op_clv(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  (void)bus;

  cpu_set_flag(cpu, FLAG_V, false);
  cpu->cycles += 2;
}

static void op_sec(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  (void)bus;
  cpu_set_flag(cpu, FLAG_C, true);
  cpu->cycles += 2;
}

static void op_sed(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  (void)bus;

  cpu_set_flag(cpu, FLAG_D, true);
  cpu->cycles += 2;
}

static void op_sei(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  (void)bus;

  cpu_set_flag(cpu, FLAG_I, true);
  cpu->cycles += 2;
}

static void op_compare(CPU *cpu, u8 reg, u8 value) {
  u16 result = (u16)reg - (u16)value;

  cpu_set_flag(cpu, FLAG_C, reg >= value);
  cpu_update_zero_and_negative_flags(cpu, (u8)result);
}

static void op_cmp_imm(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u8 value = fetch8(cpu, bus);
  op_compare(cpu, cpu->A, value);
  cpu->cycles += 2;
}

static void op_cpx_imm(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u8 value = fetch8(cpu, bus);
  op_compare(cpu, cpu->X, value);
  cpu->cycles += 2;
}

static void op_cpy_imm(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u8 value = fetch8(cpu, bus);
  op_compare(cpu, cpu->Y, value);
  cpu->cycles += 2;
}

static void op_inx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;
  (void)opcode;

  cpu->X++;
  cpu_update_zero_and_negative_flags(cpu, cpu->X);

  cpu->cycles += 2;
}

static void op_dex(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;
  (void)opcode;

  cpu->X--;
  cpu_update_zero_and_negative_flags(cpu, cpu->X);

  cpu->cycles += 2;
}

static void op_iny(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;
  (void)opcode;

  cpu->Y++;
  cpu_update_zero_and_negative_flags(cpu, cpu->Y);

  cpu->cycles += 2;
}

static void op_dey(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;
  (void)opcode;

  cpu->Y--;
  cpu_update_zero_and_negative_flags(cpu, cpu->Y);

  cpu->cycles += 2;
}

static void op_pha(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  // Push A onto the stack
  bus->write(bus->self, 0x0100 + cpu->SP--, cpu->A);
  cpu->cycles += 3; // PHA takes 3 cycles
}

static void op_pla(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  cpu->A = bus->read(bus->self, 0x0100 + ++cpu->SP);
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += 4; // PLA takes 4 cycles
}

static void op_plp(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  cpu->P = bus->read(bus->self, 0x0100 + ++cpu->SP);
  cpu->cycles += 4; // PLP takes 4 cycles
}

static void op_bit_zp(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u8 addr = fetch8(cpu, bus);
  u8 value = bus->read(bus->self, addr);

  cpu_set_flag(cpu, FLAG_Z, (cpu->A & value) == 0);
  cpu_set_flag(cpu, FLAG_N, value & 0x80);
  cpu_set_flag(cpu, FLAG_V, value & 0x40);

  cpu->cycles += 3;
}

static void op_bit_abs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;

  u16 addr = fetch16(cpu, bus);
  u8 value = bus->read(bus->self, addr);

  cpu_set_flag(cpu, FLAG_Z, (cpu->A & value) == 0);
  cpu_set_flag(cpu, FLAG_N, value & 0x80);
  cpu_set_flag(cpu, FLAG_V, value & 0x40);

  cpu->cycles += 4;
}

static void op_branch(CPU *cpu, Bus *bus, u8 opcode) {
  // bits [7:6] = flag (00=N,01=V,10=C,11=Z), bit 5 = polarity (0=clear,1=set)
  static const u8 flag_bits[] = {FLAG_N, FLAG_V, FLAG_C, FLAG_Z};
  u8 flag = flag_bits[(opcode >> 6) & 3];
  u8 cond = (opcode >> 5) & 1;
  i8 offset = (i8)fetch8(cpu, bus);
  u8 is_set = cpu->P & flag;

  // Increment PC cycles if taken
  if (cond ? is_set : !is_set) {
    u16 addr = cpu->PC + offset;
    cpu->cycles += 1;
    if ((addr ^ cpu->PC) & 0xFF00)
      cpu->cycles += 1;
    cpu->PC = addr;
  }

  cpu->cycles += 2;
}

// Transfers

static void op_tax(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;
  (void)opcode;
  cpu->X = cpu->A;
  cpu_update_zero_and_negative_flags(cpu, cpu->X);
  cpu->cycles += 2;
}

static void op_tay(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;
  (void)opcode;
  cpu->Y = cpu->A;
  cpu_update_zero_and_negative_flags(cpu, cpu->Y);
  cpu->cycles += 2;
}

static void op_tsx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;
  (void)opcode;
  cpu->X = cpu->SP;
  cpu_update_zero_and_negative_flags(cpu, cpu->X);
  cpu->cycles += 2;
}

static void op_txa(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;
  (void)opcode;
  cpu->A = cpu->X;
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += 2;
}

static void op_txs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;
  (void)opcode;
  cpu->SP = cpu->X;
  cpu->cycles += 2;
}

static void op_tya(CPU *cpu, Bus *bus, u8 opcode) {
  (void)bus;
  (void)opcode;
  cpu->A = cpu->Y;
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += 2;
}

// Stack

static void op_jsr(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u16 addr = fetch16(cpu, bus);
  u16 ret = cpu->PC - 1;
  bus->write(bus->self, 0x100 | cpu->SP, ret >> 8);
  cpu->SP--;
  bus->write(bus->self, 0x100 | cpu->SP, ret & 0xFF);
  cpu->SP--;
  cpu->PC = addr;
  cpu->cycles += 6;
}

static void op_rts(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  cpu->SP++;
  u16 lo = bus->read(bus->self, 0x100 | cpu->SP);
  cpu->SP++;
  u16 hi = bus->read(bus->self, 0x100 | cpu->SP);
  cpu->PC = (hi << 8) | lo;
  cpu->PC++;
  cpu->cycles += 6;
}

static void op_rti(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  cpu->SP++;
  cpu->P = bus->read(bus->self, 0x100 | cpu->SP);
  cpu->P |= FLAG_U;
  cpu->SP++;
  u16 lo = bus->read(bus->self, 0x100 | cpu->SP);
  cpu->SP++;
  u16 hi = bus->read(bus->self, 0x100 | cpu->SP);
  cpu->PC = (hi << 8) | lo;
  cpu->cycles += 6;
}

// STA remaining modes

static void op_sta_zpx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  bus->write(bus->self, (fetch8(cpu, bus) + cpu->X) & 0xFF, cpu->A);
  cpu->cycles += 4;
}

static void op_sta_absx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  bus->write(bus->self, fetch16(cpu, bus) + cpu->X, cpu->A);
  cpu->cycles += 5;
}

static void op_sta_absy(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  bus->write(bus->self, fetch16(cpu, bus) + cpu->Y, cpu->A);
  cpu->cycles += 5;
}

static void op_sta_indx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 zaddr = fetch8(cpu, bus) + cpu->X;
  u16 addr = bus->read(bus->self, zaddr) |
             (bus->read(bus->self, (u8)(zaddr + 1)) << 8);
  bus->write(bus->self, addr, cpu->A);
  cpu->cycles += 6;
}

static void op_sta_indy(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 zaddr = fetch8(cpu, bus);
  u16 addr = (bus->read(bus->self, zaddr) |
              (bus->read(bus->self, (u8)(zaddr + 1)) << 8)) +
             cpu->Y;
  bus->write(bus->self, addr, cpu->A);
  cpu->cycles += 6;
}

// STX remaining modes

static void op_stx_zp(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  bus->write(bus->self, fetch8(cpu, bus), cpu->X);
  cpu->cycles += 3;
}

static void op_stx_zpy(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  bus->write(bus->self, (fetch8(cpu, bus) + cpu->Y) & 0xFF, cpu->X);
  cpu->cycles += 4;
}

// STY remaining modes

static void op_sty_zp(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  bus->write(bus->self, fetch8(cpu, bus), cpu->Y);
  cpu->cycles += 3;
}

static void op_sty_zpx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  bus->write(bus->self, (fetch8(cpu, bus) + cpu->X) & 0xFF, cpu->Y);
  cpu->cycles += 4;
}

// LDA remaining modes

static void op_lda_zpx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 addr = (fetch8(cpu, bus) + cpu->X) & 0xFF;
  cpu->A = bus->read(bus->self, addr);
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += 4;
}

static void op_lda_absx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u16 addr = fetch16(cpu, bus);
  u16 ea = addr + cpu->X;
  cpu->A = bus->read(bus->self, ea);
  if ((ea & 0xFF00) != (addr & 0xFF00))
    cpu->cycles++;
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += 4;
}

static void op_lda_absy(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u16 addr = fetch16(cpu, bus);
  u16 ea = addr + cpu->Y;
  cpu->A = bus->read(bus->self, ea);
  if ((ea & 0xFF00) != (addr & 0xFF00))
    cpu->cycles++;
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += 4;
}

static void op_lda_indx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 zaddr = fetch8(cpu, bus) + cpu->X;
  u16 addr = bus->read(bus->self, zaddr) |
             (bus->read(bus->self, (u8)(zaddr + 1)) << 8);
  cpu->A = bus->read(bus->self, addr);
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += 6;
}

static void op_lda_indy(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 zaddr = fetch8(cpu, bus);
  u16 base = bus->read(bus->self, zaddr) |
             (bus->read(bus->self, (u8)(zaddr + 1)) << 8);
  u16 addr = base + cpu->Y;
  cpu->A = bus->read(bus->self, addr);
  if ((addr & 0xFF00) != (base & 0xFF00))
    cpu->cycles++;
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += 5;
}

static void op_ldx_abs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  cpu->X = bus->read(bus->self, fetch16(cpu, bus));
  cpu_update_zero_and_negative_flags(cpu, cpu->X);
  cpu->cycles += 4;
}

static void op_ldx_absy(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u16 addr = fetch16(cpu, bus);
  u16 ea = addr + cpu->Y;
  cpu->X = bus->read(bus->self, ea);
  if ((ea & 0xFF00) != (addr & 0xFF00))
    cpu->cycles++;
  cpu_update_zero_and_negative_flags(cpu, cpu->X);
  cpu->cycles += 4;
}

// LDY remaining modes

static void op_ldy_abs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  cpu->Y = bus->read(bus->self, fetch16(cpu, bus));
  cpu_update_zero_and_negative_flags(cpu, cpu->Y);
  cpu->cycles += 4;
}

static void op_ldy_zpx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 addr = (fetch8(cpu, bus) + cpu->X) & 0xFF;
  cpu->Y = bus->read(bus->self, addr);
  cpu_update_zero_and_negative_flags(cpu, cpu->Y);
  cpu->cycles += 4;
}

static void op_ldy_absx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u16 addr = fetch16(cpu, bus);
  u16 ea = addr + cpu->X;
  cpu->Y = bus->read(bus->self, ea);
  if ((ea & 0xFF00) != (addr & 0xFF00))
    cpu->cycles++;
  cpu_update_zero_and_negative_flags(cpu, cpu->Y);
  cpu->cycles += 4;
}

// CMP remaining modes

static void op_cmp_zp(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 value = bus->read(bus->self, fetch8(cpu, bus));
  op_compare(cpu, cpu->A, value);
  cpu->cycles += 3;
}

static void op_cmp_abs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 value = bus->read(bus->self, fetch16(cpu, bus));
  op_compare(cpu, cpu->A, value);
  cpu->cycles += 4;
}

static void op_cmp_indx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 zaddr = fetch8(cpu, bus) + cpu->X;
  u16 addr = bus->read(bus->self, zaddr) |
             (bus->read(bus->self, (u8)(zaddr + 1)) << 8);
  u8 value = bus->read(bus->self, addr);
  op_compare(cpu, cpu->A, value);
  cpu->cycles += 6;
}

static void op_cmp_indy(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 zaddr = fetch8(cpu, bus);
  u16 base = bus->read(bus->self, zaddr) |
             (bus->read(bus->self, (u8)(zaddr + 1)) << 8);
  u16 addr = base + cpu->Y;
  u8 value = bus->read(bus->self, addr);
  if ((addr & 0xFF00) != (base & 0xFF00))
    cpu->cycles++;
  op_compare(cpu, cpu->A, value);
  cpu->cycles += 5;
}

static void op_cmp_zpx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 addr = (fetch8(cpu, bus) + cpu->X) & 0xFF;
  u8 value = bus->read(bus->self, addr);
  op_compare(cpu, cpu->A, value);
  cpu->cycles += 4;
}

static void op_cmp_absy(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u16 addr = fetch16(cpu, bus);
  u16 ea = addr + cpu->Y;
  u8 value = bus->read(bus->self, ea);
  if ((ea & 0xFF00) != (addr & 0xFF00))
    cpu->cycles++;
  op_compare(cpu, cpu->A, value);
  cpu->cycles += 4;
}

static void op_cmp_absx(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u16 addr = fetch16(cpu, bus);
  u16 ea = addr + cpu->X;
  u8 value = bus->read(bus->self, ea);
  if ((ea & 0xFF00) != (addr & 0xFF00))
    cpu->cycles++;
  op_compare(cpu, cpu->A, value);
  cpu->cycles += 4;
}

// CPX remaining modes

static void op_cpx_zp(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 value = bus->read(bus->self, fetch8(cpu, bus));
  op_compare(cpu, cpu->X, value);
  cpu->cycles += 3;
}

static void op_cpx_abs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 value = bus->read(bus->self, fetch16(cpu, bus));
  op_compare(cpu, cpu->X, value);
  cpu->cycles += 4;
}

// CPY remaining modes

static void op_cpy_zp(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 value = bus->read(bus->self, fetch8(cpu, bus));
  op_compare(cpu, cpu->Y, value);
  cpu->cycles += 3;
}

static void op_cpy_abs(CPU *cpu, Bus *bus, u8 opcode) {
  (void)opcode;
  u8 value = bus->read(bus->self, fetch16(cpu, bus));
  op_compare(cpu, cpu->Y, value);
  cpu->cycles += 4;
}

// Shared opcode-group handlers

static void op_rmw(CPU *cpu, Bus *bus, u8 opcode) {
  u8 value;
  u16 addr = 0;
  u8 cycles;

  if ((opcode & 0x0F) == 0x0A) {
    value = cpu->A;
    cycles = 2;
  } else {
    u8 idx = (opcode >> 4) & 1;
    if ((opcode & 0x0F) == 0x06) {
      addr = (fetch8(cpu, bus) + (idx ? cpu->X : 0)) & 0xFF;
      cycles = 5 + idx;
    } else { // SBC
      addr = fetch16(cpu, bus) + (idx ? cpu->X : 0);
      cycles = 6 + idx;
    }
    value = bus->read(bus->self, addr);
  }

  switch ((opcode >> 5) & 7) {
  case 0: // ASL
    cpu_set_flag(cpu, FLAG_C, value & 0x80);
    value <<= 1;
    break;
  case 1: { // ROL
    u8 c = value >> 7;
    value = (value << 1) | (cpu->P & FLAG_C);
    cpu_set_flag(cpu, FLAG_C, c);
    break;
  }
  case 2: // LSR
    cpu_set_flag(cpu, FLAG_C, value & 1);
    value >>= 1;
    break;
  case 3: { // ROR
    u8 c = value & 1;
    value = (value >> 1) | (cpu->P & FLAG_C ? 0x80 : 0);
    cpu_set_flag(cpu, FLAG_C, c);
    break;
  }
  case 6: // DEC
    value--;
    break;
  case 7: // INC
    value++;
    break;
  }

  if ((opcode & 0x0F) == 0x0A)
    cpu->A = value;
  else
    bus->write(bus->self, addr, value);

  cpu_update_zero_and_negative_flags(cpu, value);
  cpu->cycles += cycles;
}

static void op_logic(CPU *cpu, Bus *bus, u8 opcode) {
  u8 value;
  u8 cycles;

  switch (opcode & 0x1F) {
  case 0x01: { // (zp,X)
    u8 zpage = fetch8(cpu, bus) + cpu->X;
    value =
        bus->read(bus->self, bus->read(bus->self, zpage) |
                                 (bus->read(bus->self, (u8)(zpage + 1)) << 8));
    cycles = 6;
    break;
  }
  case 0x11: { // (zp),Y
    u8 zpage = fetch8(cpu, bus);
    u16 base = bus->read(bus->self, zpage) |
               (bus->read(bus->self, (u8)(zpage + 1)) << 8);
    u16 ea = base + cpu->Y;
    value = bus->read(bus->self, ea);
    cycles = 5 + ((ea ^ base) >> 8);
    break;
  }
  case 0x09: // #imm
    value = fetch8(cpu, bus);
    cycles = 2;
    break;
  case 0x05: // zp
    value = bus->read(bus->self, fetch8(cpu, bus));
    cycles = 3;
    break;
  case 0x15: // zp,X
    value = bus->read(bus->self, (fetch8(cpu, bus) + cpu->X) & 0xFF);
    cycles = 4;
    break;
  case 0x0D: // abs
    value = bus->read(bus->self, fetch16(cpu, bus));
    cycles = 4;
    break;
  case 0x1D: { // abs,X
    u16 ba = fetch16(cpu, bus);
    u16 ea = ba + cpu->X;
    value = bus->read(bus->self, ea);
    cycles = 4 + ((ea ^ ba) >> 8);
    break;
  }
  case 0x19: { // abs,Y
    u16 ba = fetch16(cpu, bus);
    u16 ea = ba + cpu->Y;
    value = bus->read(bus->self, ea);
    cycles = 4 + ((ea ^ ba) >> 8);
    break;
  }
  default: { // (zp),Y
    u8 zpage = fetch8(cpu, bus);
    u16 base = bus->read(bus->self, zpage) |
               (bus->read(bus->self, (u8)(zpage + 1)) << 8);
    u16 ea = base + cpu->Y;
    value = bus->read(bus->self, ea);
    cycles = 5 + ((ea ^ base) >> 8);
    break;
  }
  }

  switch ((opcode >> 5) & 3) {
  case 0: // ORA
    cpu->A |= value;
    break;
  case 1: // AND
    cpu->A &= value;
    break;
  case 2: // EOR
    cpu->A ^= value;
    break;
  }
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += cycles;
}

static void op_adc_sbc(CPU *cpu, Bus *bus, u8 opcode) {
  u8 value;
  u8 cycles;

  switch (opcode & 0x1F) {
  case 0x01: { // (zp,X)
    u8 zpage = fetch8(cpu, bus) + cpu->X;
    value =
        bus->read(bus->self, bus->read(bus->self, zpage) |
                                 (bus->read(bus->self, (u8)(zpage + 1)) << 8));
    cycles = 6;
    break;
  }
  case 0x11: { // (zp),Y
    u8 zpage = fetch8(cpu, bus);
    u16 base = bus->read(bus->self, zpage) |
               (bus->read(bus->self, (u8)(zpage + 1)) << 8);
    u16 ea = base + cpu->Y;
    value = bus->read(bus->self, ea);
    cycles = 5 + ((ea ^ base) >> 8);
    break;
  }
  case 0x09: // #imm
    value = fetch8(cpu, bus);
    cycles = 2;
    break;
  case 0x05: // zp
    value = bus->read(bus->self, fetch8(cpu, bus));
    cycles = 3;
    break;
  case 0x15: // zp,X
    value = bus->read(bus->self, (fetch8(cpu, bus) + cpu->X) & 0xFF);
    cycles = 4;
    break;
  case 0x0D: // abs
    value = bus->read(bus->self, fetch16(cpu, bus));
    cycles = 4;
    break;
  case 0x1D: { // abs,X
    u16 ba = fetch16(cpu, bus);
    u16 ea = ba + cpu->X;
    value = bus->read(bus->self, ea);
    cycles = 4 + ((ea ^ ba) >> 8);
    break;
  }
  case 0x19: { // abs,Y
    u16 ba = fetch16(cpu, bus);
    u16 ea = ba + cpu->Y;
    value = bus->read(bus->self, ea);
    cycles = 4 + ((ea ^ ba) >> 8);
    break;
  }
  default: { // (zp),Y
    u8 zpage = fetch8(cpu, bus);
    u16 base = bus->read(bus->self, zpage) |
               (bus->read(bus->self, (u8)(zpage + 1)) << 8);
    u16 ea = base + cpu->Y;
    value = bus->read(bus->self, ea);
    cycles = 5 + ((ea ^ base) >> 8);
    break;
  }
  }

  if (!(opcode & 0x80)) { // ADC
    u16 result = (u16)cpu->A + (u16)value + (u16)(cpu->P & FLAG_C);
    cpu_set_flag(cpu, FLAG_C, result > 0xFF);
    cpu_set_flag(cpu, FLAG_V,
                 (u8)(~(cpu->A ^ value) & (cpu->A ^ (u8)result) & 0x80));
    cpu->A = (u8)result;
  } else { // SBC
    u16 borrow = (cpu->P & FLAG_C) ? 0 : 1;
    u16 diff = (u16)cpu->A - (u16)value - borrow;
    cpu_set_flag(cpu, FLAG_C, diff < 0x100);
    cpu_set_flag(cpu, FLAG_V,
                 (u8)((cpu->A ^ value) & (cpu->A ^ (u8)diff) & 0x80));
    cpu->A = (u8)diff;
  }
  cpu_update_zero_and_negative_flags(cpu, cpu->A);
  cpu->cycles += cycles;
}

static OpcodeHandler const opcode_table[256] = {
    [0x00] = op_brk,      [0x01] = op_logic,    [0x05] = op_logic,
    [0x06] = op_rmw,      [0x08] = op_php,      [0x09] = op_logic,
    [0x0A] = op_rmw,      [0x0D] = op_logic,    [0x0E] = op_rmw,
    [0x10] = op_branch,   [0x11] = op_logic,    [0x15] = op_logic,
    [0x16] = op_rmw,      [0x18] = op_clc,      [0x19] = op_logic,
    [0x1D] = op_logic,    [0x1E] = op_rmw,      [0x20] = op_jsr,
    [0x21] = op_logic,    [0x24] = op_bit_zp,   [0x25] = op_logic,
    [0x26] = op_rmw,      [0x28] = op_plp,      [0x29] = op_logic,
    [0x2A] = op_rmw,      [0x2C] = op_bit_abs,  [0x2D] = op_logic,
    [0x2E] = op_rmw,      [0x30] = op_branch,   [0x31] = op_logic,
    [0x35] = op_logic,    [0x36] = op_rmw,      [0x38] = op_sec,
    [0x39] = op_logic,    [0x3D] = op_logic,    [0x3E] = op_rmw,
    [0x40] = op_rti,      [0x41] = op_logic,    [0x45] = op_logic,
    [0x46] = op_rmw,      [0x48] = op_pha,      [0x49] = op_logic,
    [0x4A] = op_rmw,      [0x4C] = op_jmp_abs,  [0x4D] = op_logic,
    [0x4E] = op_rmw,      [0x50] = op_branch,   [0x51] = op_logic,
    [0x55] = op_logic,    [0x56] = op_rmw,      [0x58] = op_cli,
    [0x59] = op_logic,    [0x5D] = op_logic,    [0x5E] = op_rmw,
    [0x60] = op_rts,      [0x61] = op_adc_sbc,  [0x65] = op_adc_sbc,
    [0x66] = op_rmw,      [0x68] = op_pla,      [0x69] = op_adc_sbc,
    [0x6A] = op_rmw,      [0x6C] = op_jmp_ind,  [0x6D] = op_adc_sbc,
    [0x6E] = op_rmw,      [0x70] = op_branch,   [0x71] = op_adc_sbc,
    [0x75] = op_adc_sbc,  [0x76] = op_rmw,      [0x78] = op_sei,
    [0x79] = op_adc_sbc,  [0x7D] = op_adc_sbc,  [0x7E] = op_rmw,
    [0x81] = op_sta_indx, [0x84] = op_sty_zp,   [0x85] = op_sta_zp,
    [0x86] = op_stx_zp,   [0x88] = op_dey,      [0x8A] = op_txa,
    [0x8C] = op_sty_abs,  [0x8D] = op_sta_abs,  [0x8E] = op_stx_abs,
    [0x90] = op_branch,   [0x91] = op_sta_indy, [0x94] = op_sty_zpx,
    [0x95] = op_sta_zpx,  [0x96] = op_stx_zpy,  [0x98] = op_tya,
    [0x99] = op_sta_absy, [0x9A] = op_txs,      [0x9D] = op_sta_absx,
    [0xA0] = op_ldy_imm,  [0xA1] = op_lda_indx, [0xA2] = op_ldx_imm,
    [0xA4] = op_ldy_zp,   [0xA5] = op_lda_zp,   [0xA6] = op_ldx_zp,
    [0xA8] = op_tay,      [0xA9] = op_lda_imm,  [0xAA] = op_tax,
    [0xAC] = op_ldy_abs,  [0xAD] = op_lda_abs,  [0xAE] = op_ldx_abs,
    [0xB0] = op_branch,   [0xB1] = op_lda_indy, [0xB4] = op_ldy_zpx,
    [0xB5] = op_lda_zpx,  [0xB6] = op_ldx_zp,   [0xB8] = op_clv,
    [0xB9] = op_lda_absy, [0xBA] = op_tsx,      [0xBC] = op_ldy_absx,
    [0xBD] = op_lda_absx, [0xBE] = op_ldx_absy, [0xC0] = op_cpy_imm,
    [0xC1] = op_cmp_indx, [0xC4] = op_cpy_zp,   [0xC5] = op_cmp_zp,
    [0xC6] = op_rmw,      [0xC8] = op_iny,      [0xC9] = op_cmp_imm,
    [0xCA] = op_dex,      [0xCC] = op_cpy_abs,  [0xCD] = op_cmp_abs,
    [0xCE] = op_rmw,      [0xD0] = op_branch,   [0xD1] = op_cmp_indy,
    [0xD5] = op_cmp_zpx,  [0xD6] = op_rmw,      [0xD8] = op_cld,
    [0xD9] = op_cmp_absy, [0xDD] = op_cmp_absx, [0xDE] = op_rmw,
    [0xE0] = op_cpx_imm,  [0xE1] = op_adc_sbc,  [0xE4] = op_cpx_zp,
    [0xE5] = op_adc_sbc,  [0xE6] = op_rmw,      [0xE8] = op_inx,
    [0xE9] = op_adc_sbc,  [0xEA] = op_nop,      [0xEC] = op_cpx_abs,
    [0xED] = op_adc_sbc,  [0xEE] = op_rmw,      [0xF0] = op_branch,
    [0xF1] = op_adc_sbc,  [0xF5] = op_adc_sbc,  [0xF6] = op_rmw,
    [0xF8] = op_sed,      [0xF9] = op_adc_sbc,  [0xFD] = op_adc_sbc,
    [0xFE] = op_rmw,
};

static void cpu_step(CPU *cpu, Bus *bus) {
  if (bus->nmi_pending) {
    cpu_nmi(cpu, bus);
  } else if (bus->irq_pending && !(cpu->P & FLAG_I)) {
    cpu_irq(cpu, bus);
  }

  u8 opcode = fetch8(cpu, bus);

  OpcodeHandler handler = opcode_table[opcode];

  if (handler) {
    handler(cpu, bus, opcode);
  } else {
    op_illegal(cpu, bus, opcode);
  }
}

#endif
