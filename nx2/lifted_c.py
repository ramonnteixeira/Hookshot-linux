"""Where the lifted functions begin in the lift's C (code_*.c): each definition is one line, `void f_<address>(Ctx *__restrict
A64_NOESC c) {`, its name in group 1. Small on purpose: nx2/build.py reads it for every compile, without the lifter.

HEAD                 the definitions alone (split_hot: a piece takes each function from its own line)
HEAD_NOTED           with the /* ... */ line the lift may put just above one (chunks: a part keeps a function's note with it)
HEAD_BYTES           HEAD, for bytes (build.py's memory estimate reads the files as they lie)

The lift writes its C with \n line ends; a file with \r\n ones (written in text mode on Windows) is read the same: none of the
patterns needs the end of the definition's line, and a note's line may end in \r\n.
"""
import re

_DEF = r"void (f_[0-9a-f]+)\(Ctx \*__restrict A64_NOESC c\) \{"
HEAD = re.compile(r"^" + _DEF, re.M)
HEAD_NOTED = re.compile(r"^(?:/\* .* \*/\r?\n)?" + _DEF, re.M)
HEAD_BYTES = re.compile((r"^" + _DEF).encode(), re.M)
