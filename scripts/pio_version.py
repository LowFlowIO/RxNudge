# PlatformIO pre-build: defines MT_FW_VERSION from the VERSION file (the single
# source; CMakeLists.txt reads the same file for PROJECT_VER).
import os
Import("env")  # noqa: F821

root = env.subst("$PROJECT_DIR")  # noqa: F821
with open(os.path.join(root, "VERSION")) as f:
    version = f.readline().strip()
env.Append(CPPDEFINES=[("MT_FW_VERSION", env.StringifyMacro(version))])  # noqa: F821
print(f"RxNudge firmware version {version}")
