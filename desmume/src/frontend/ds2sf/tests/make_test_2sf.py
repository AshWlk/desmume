#!/usr/bin/env python3
"""Builds the ds2sf test fixtures from arm9.s / arm7.s.

Writes to OUTDIR:
  ds2sf_test.2sflib  - the whole test ROM as one 2SF program section (plays 439.95 Hz)
  ds2sf_test.mini2sf - _lib=ds2sf_test.2sflib, plus a 4-byte section patching the ARM9 command
                       word to 0x880 (plays 880.09 Hz). Exercises the lib-chain overlay.

The ROM uses a retail-style layout (ARM9 at 0x4000 behind a decrypted secure-area marker, ARM7 at
0x8000) so it is validated and booted the way ripped game ROMs are, not via the homebrew path.

Requires llvm-mc, llvm-objcopy, llvm-objdump and llvm-nm (any LLVM with the ARM target).
usage: make_test_2sf.py OUTDIR
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

ARM9_ROM_OFFSET = 0x4000
ARM9_RAM_ADDR = 0x02000000
ARM9_ENTRY_OFFSET = 0x800      # code starts after the 2 KiB secure area, as on retail cards
ARM7_ROM_OFFSET = 0x8000
ARM7_RAM_ADDR = 0x02380000
ROM_SIZE = 0x10000


def assemble(source, triple, workdir):
    obj = os.path.join(workdir, os.path.basename(source) + '.o')
    binary = os.path.join(workdir, os.path.basename(source) + '.bin')
    subprocess.run(['llvm-mc', f'-triple={triple}', '-filetype=obj', '-o', obj, source], check=True)
    # The objects are flattened with objcopy, not linked, so any relocation would be silently left
    # unapplied in the binary (e.g. a PC-relative load of a .global label).
    relocs = subprocess.run(['llvm-objdump', '-r', obj], check=True, capture_output=True, text=True).stdout
    if 'R_ARM_' in relocs:
        sys.exit(f'{source}: unresolved relocations; keep labels local:\n{relocs}')
    subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', obj, binary], check=True)
    symbols = subprocess.run(['llvm-nm', obj], check=True, capture_output=True, text=True).stdout
    offsets = {}
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) == 3:
            offsets[parts[2]] = int(parts[0], 16)
    with open(binary, 'rb') as f:
        return f.read(), offsets


def align4(n):
    return (n + 3) & ~3


def build_rom(arm9, arm7):
    rom = bytearray(ROM_SIZE)
    arm9_size = ARM9_ENTRY_OFFSET + align4(len(arm9))
    arm7_size = align4(len(arm7))

    header = bytearray(0x200)
    header[0x000:0x00C] = b'DS2SF TEST\0\0'
    header[0x00C:0x010] = b'ZDST'
    header[0x010:0x012] = b'00'
    header[0x012] = 0      # unit code: NDS
    header[0x014] = 0      # device capacity: 128 KiB << 0
    struct.pack_into('<IIII', header, 0x020, ARM9_ROM_OFFSET, ARM9_RAM_ADDR + ARM9_ENTRY_OFFSET, ARM9_RAM_ADDR, arm9_size)
    struct.pack_into('<IIII', header, 0x030, ARM7_ROM_OFFSET, ARM7_RAM_ADDR, ARM7_RAM_ADDR, arm7_size)
    struct.pack_into('<I', header, 0x080, ROM_SIZE)   # total used ROM size
    struct.pack_into('<I', header, 0x084, 0x4000)     # header size
    rom[0:0x200] = header

    # A decrypted ("dumped") secure area starts with this marker.
    rom[ARM9_ROM_OFFSET:ARM9_ROM_OFFSET + 8] = bytes.fromhex('FFDEFFE7FFDEFFE7')
    entry = ARM9_ROM_OFFSET + ARM9_ENTRY_OFFSET
    rom[entry:entry + len(arm9)] = arm9
    rom[ARM7_ROM_OFFSET:ARM7_ROM_OFFSET + len(arm7)] = arm7
    return bytes(rom)


def write_psf(path, program, tags):
    compressed = zlib.compress(program, 9)
    with open(path, 'wb') as f:
        f.write(b'PSF\x24')
        f.write(struct.pack('<III', 0, len(compressed), zlib.crc32(compressed)))
        f.write(compressed)
        if tags:
            f.write(b'[TAG]')
            for key, value in tags.items():
                f.write(f'{key}={value}\n'.encode('utf-8'))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)

    with tempfile.TemporaryDirectory() as work:
        arm9, arm9_symbols = assemble(os.path.join(HERE, 'arm9.s'), 'armv5te-none-eabi', work)
        arm7, _ = assemble(os.path.join(HERE, 'arm7.s'), 'armv4t-none-eabi', work)

    rom = build_rom(arm9, arm7)
    command_rom_offset = ARM9_ROM_OFFSET + ARM9_ENTRY_OFFSET + arm9_symbols['command']
    assert struct.unpack_from('<I', rom, command_rom_offset)[0] == 0x440

    write_psf(os.path.join(outdir, 'ds2sf_test.2sflib'),
              struct.pack('<II', 0, len(rom)) + rom,
              {'title': 'ds2sf test lib (440 Hz)', 'length': '0:05'})

    write_psf(os.path.join(outdir, 'ds2sf_test.mini2sf'),
              struct.pack('<II', command_rom_offset, 4) + struct.pack('<I', 0x880),
              {'_lib': 'ds2sf_test.2sflib', 'title': 'ds2sf test (880 Hz)', 'length': '0:05'})

    print(f'wrote ds2sf_test.2sflib and ds2sf_test.mini2sf to {outdir} '
          f'(command word at ROM 0x{command_rom_offset:X})')


if __name__ == '__main__':
    main()
