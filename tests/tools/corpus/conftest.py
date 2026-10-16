"""pytest setup for the corpus tooling tests (docs/CORPUS.md, "Testing").

Run from the repository root:  python -I -m pytest tests/tools/corpus -q
Optional: VS_EVAL=<path to vs-eval> also runs the end-to-end test through vs-eval.
"""

from __future__ import annotations

import os
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
CORPUS_DIR = os.path.join(REPO, "tools", "corpus")
if CORPUS_DIR not in sys.path:
    sys.path.insert(0, CORPUS_DIR)
