#!/usr/bin/env python3
"""Run the PRG32 CLI with the 64 KiB portable cartridge-RAM fallback.

PRG32 commit 596bcf9 raised the firmware's default cartridge RAM and package
limit to 64 KiB, but the portable builder still falls back to 32 KiB when it
has no runtime to ask (prg32/utilities/env_variables.py). This adapter only
patches that fallback for the current process; it changes no firmware or
package-format behaviour. Run it from the PRG32 checkout root.
"""

import importlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path.cwd()))

from prg32.utilities import env_variables  # noqa: E402

RAM_SIZE = 64 * 1024
env_variables.FALLBACK_CART_RAM_SIZE = RAM_SIZE
builder = importlib.import_module("prg32.cartridge.build_cartridge")
builder.FALLBACK_CART_RAM_SIZE = RAM_SIZE
runtime = importlib.import_module("prg32.utilities.runtime_handler")
runtime.FALLBACK_CART_RAM_SIZE = RAM_SIZE

from prg32 import prg32  # noqa: E402

prg32.main(sys.argv[1:])
