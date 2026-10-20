# PlatformIO post-build: writes firmware.bin.sha256 next to firmware.bin.
# The web portal's OTA upload requires this value (paste it or pick the file).
import hashlib
Import("env")  # noqa: F821


def write_sha(source, target, env):
    path = str(target[0])
    digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
    with open(path + ".sha256", "w") as f:
        f.write(f"{digest}  firmware.bin\n")
    print(f"SHA-256 {digest}  ({path}.sha256)")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", write_sha)  # noqa: F821
