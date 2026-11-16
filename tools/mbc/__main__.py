from __future__ import annotations

import sys
from pathlib import Path

if __package__:
    from .cli import main
else:
    # `python3 /any/path/tools/mbc` executes this file without a package.
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.mbc.cli import main

raise SystemExit(main())
