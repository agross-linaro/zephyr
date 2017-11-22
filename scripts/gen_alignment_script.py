#!/usr/bin/env python3
#
# Copyright (c) 2017 Intel Corporation
#
# SPDX-License-Identifier: Apache-2.0

import sys
import argparse
import pprint
import os
import struct
from distutils.version import LooseVersion

import elftools
from elftools.elf.elffile import ELFFile
from elftools.dwarf import descriptions
from elftools.elf.sections import SymbolTableSection

if LooseVersion(elftools.__version__) < LooseVersion('0.24'):
    sys.stderr.write("pyelftools is out of date, need version 0.24 or later\n")
    sys.exit(1)

def get_symbols(obj):
    for section in obj.iter_sections():
        if isinstance(section, SymbolTableSection):
            return {sym.name: sym.entry.st_value
                    for sym in section.iter_symbols()}

    raise LookupError("Could not find symbol table")

def parse_args():
    global args

    parser = argparse.ArgumentParser(description = __doc__,
            formatter_class = argparse.RawDescriptionHelpFormatter)

    parser.add_argument("-k", "--kernel", required=True,
            help="Input zephyr ELF binary")
    parser.add_argument("-l", "--linker", required=True,
            help="Output linker file")
    parser.add_argument("-v", "--verbose", action="store_true",
            help="Print extra debugging information")
    args = parser.parse_args()


def main():
    parse_args()

    with open(args.kernel, "rb") as fp:
        elf = ELFFile(fp)
        args.little_endian = elf.little_endian
        syms = get_symbols(elf)

    app_ram_size = syms['__app_last_address_used'] - syms['__app_ram_start']
    bit_len = app_ram_size.bit_length()

    if bit_len == 0:
        align_size = 32
    else:
        align_size = 1 << bit_len

    with open(args.linker, "r") as fp:
        contents = fp.readlines()

    for l in contents:
        if ('_app_data_align =' in l):
            sys.stdout.write(" _app_data_align = %d;\n" % align_size)
        else:
            sys.stdout.write(l)

if __name__ == "__main__":
    main()

