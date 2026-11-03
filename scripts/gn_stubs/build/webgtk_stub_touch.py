#!/usr/bin/env python3
# Stub created by scripts/patch_gn_stubs.sh for the pruned engine archive.
# Stub helper for the pruned engine archive.
#
# A few grit() targets in the real tree read a .grd/.js file that another
# target generates. The archive drops those generators, and `gn gen` then
# refuses to parse ("no targets in the build generate that file"). Stub
# generators are actions that call this script, which creates the placeholder
# files so the dependency graph is well-formed. Nothing builds them.
import os
import sys

for path in sys.argv[1:]:
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    open(path, 'w').close()
