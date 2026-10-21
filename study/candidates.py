"""Candidate seeded faults for the click study, taken from real lines of click 8.x
source. A candidate is kept only if its old text occurs exactly once in the file
and at least one existing test fails when it is applied (so every task is caught
by the suite)."""

CANDIDATES = [
    ("src/click/utils.py", "    if len(text) <= max_length:", "    if len(text) < max_length:"),
    ("src/click/utils.py", '    if max_length < len("..."):', '    if max_length <= len("..."):'),
    ("src/click/utils.py", "    if not words:", "    if words:"),
    ("src/click/utils.py", "    if shorten:", "    if not shorten:"),
    ("src/click/types.py", "        if self.clamp:", "        if not self.clamp:"),
    ("src/click/types.py", "        if self.min is None:", "        if self.min is not None:"),
    ("src/click/types.py", "        if self.max is None:", "        if self.max is not None:"),
    ("src/click/types.py", "        if self.case_sensitive:", "        if not self.case_sensitive:"),
    ("src/click/core.py", "        if self.nargs == -1:", "        if self.nargs == 0:"),
    ("src/click/core.py", "        if self.count:", "        if not self.count:"),
    ("src/click/core.py", "        if self.required and self.value_is_missing(value):",
     "        if not self.required and self.value_is_missing(value):"),
    ("src/click/core.py", "        if self.is_flag and not self.required and self.is_bool_flag and value is UNSET:",
     "        if self.is_flag and not self.required and self.is_bool_flag and value is None:"),
    ("src/click/parser.py", 'if arg == "--":', 'if arg == "-":'),
]
