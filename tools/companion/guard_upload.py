# PlatformIO's standard uploader also replaces the shared bootloader/partition table.
# Companion images must go through the paired installer or app-only update command.
Import("env")
from SCons.Script import COMMAND_LINE_TARGETS
if any(target in COMMAND_LINE_TARGETS for target in ("upload", "uploadfs", "erase")):
    raise RuntimeError("Use tools/companion for companion installation and updates; normal upload is disabled.")
