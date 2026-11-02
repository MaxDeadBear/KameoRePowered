"""Sets up generated/tu, the title-update build's workspace: the game executable and the update's delta patch from
assets/TU, a codegen manifest over config/tu/, and our sources translated to the update's addresses (translate.py
--sources). CMake runs it when configuring with KAMEO_TITLE_UPDATE; the codegen itself runs as part of the build.

Usage: prepare.py [--codegen] (also runs codegen now, for the tools that read the generated code)."""
import os, shutil, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WS = os.path.join(ROOT, "generated", "tu")
SDK = os.environ.get("REXGLUE_SDK", "F:/rexglue-sdk/out/install/win-amd64")


def copy_if_changed(src, dst):
    if os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src) and os.path.getmtime(dst) >= os.path.getmtime(src):
        return
    shutil.copy2(src, dst)


def main() -> int:
    patch = os.path.join(ROOT, "assets", "TU", "default.xexp")
    if not os.path.exists(patch):
        print(f"prepare.py: {patch} is missing: the title-update build needs the update's delta patch", file=sys.stderr)
        return 1
    os.makedirs(os.path.join(WS, "assets"), exist_ok=True)
    # The loader applies a sibling default.xexp to default.xex.
    copy_if_changed(os.path.join(ROOT, "assets", "default.xex"), os.path.join(WS, "assets", "default.xex"))
    copy_if_changed(patch, os.path.join(WS, "assets", "default.xexp"))
    config = os.path.join(WS, "config")
    shutil.rmtree(config, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "config", "tu"), config)
    # The hooks are missing only on the very first run, before translate.py --config has placed them.
    hooks = os.path.join(config, "hooks")
    includes = ["config/functions.toml"] + sorted(f"config/hooks/{n}" for n in (os.listdir(hooks) if os.path.isdir(hooks) else []))
    manifest = (
        "# Written by tools/tu/prepare.py for the title-update build.\n"
        "[project]\nname = \"kameorepowered\"\nsdk_version = \"0.10.0\"\ngame_root = \"assets\"\n\n"
        "[entrypoint]\nfile_path = \"assets/default.xex\"\nout_directory_path = \"generated/default\"\n"
        "includes = [\n" + "".join(f"  \"{i}\",\n" for i in includes) + "]\n")
    path = os.path.join(WS, "kameorepowered_manifest.toml")
    if not os.path.exists(path) or open(path).read() != manifest:
        open(path, "w").write(manifest)
    if "--codegen" in sys.argv:
        code = subprocess.call([os.path.join(SDK, "bin", "rexglue.exe"), "codegen", "kameorepowered_manifest.toml"], cwd=WS)
        if code:
            return code
    return subprocess.call([sys.executable, os.path.join(os.path.dirname(__file__), "translate.py"), "--sources"])


if __name__ == "__main__":
    sys.exit(main())
