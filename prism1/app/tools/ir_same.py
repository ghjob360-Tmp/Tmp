#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1.toml ir_pass: the IR unchanged (the pass exists so the build writes each unit's optimised IR, which its
stack_check reads: tools/stack_depth.py)."""
import shutil
import sys

shutil.copyfile(sys.argv[1], sys.argv[2])
