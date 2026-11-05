"""Run the production C rules as compiled Cortex-M0+ code; requires unicorn/pyelftools."""
import argparse
import pathlib
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0

parser = argparse.ArgumentParser()
parser.add_argument('--cc', required=True)
parser.add_argument('--output', required=True)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
out = pathlib.Path(args.output).resolve()
out.mkdir(parents=True, exist_ok=True)
elf_path = out/'test_rules.elf'
subprocess.run([args.cc, '-mcpu=cortex-m0plus', '-mthumb', '-std=gnu11', '-Os',
    '-Wall', '-Wextra', '-Werror', '-fno-builtin', '-ffunction-sections',
    '-fdata-sections', '-nostdlib', '-I'+str(root/'lab4/source'),
    str(root/'tests/test_rules.c'), str(root/'tests/test_support.c'),
    '-T'+str(root/'tests/test_rules.ld'), '-Wl,--gc-sections', '-lgcc', '-o', str(elf_path)], check=True)
uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(0x10000, 0x20000)
uc.mem_map(0x20000000, 0x10000)
uc.mem_map(0x30000, 0x1000)
with elf_path.open('rb') as f:
    elf = ELFFile(f)
    for seg in elf.iter_segments():
        if seg['p_type'] == 'PT_LOAD' and seg['p_filesz']:
            uc.mem_write(seg['p_vaddr'], seg.data())
    entry = elf.header['e_entry']
uc.reg_write(UC_ARM_REG_SP, 0x2000FFF0)
uc.reg_write(UC_ARM_REG_LR, 0x30001)
uc.emu_start(entry | 1, 0x30000, count=3000000)
result = uc.reg_read(UC_ARM_REG_R0)
if result:
    raise SystemExit(f'FAILED: test_rules.c line {result}')
print('PASS: ARM countdown fractions, watering/cooldown/cap, interlocks, wraparound and UART validation')
