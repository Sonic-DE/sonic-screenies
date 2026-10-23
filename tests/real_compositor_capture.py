# SPDX-FileCopyrightText: 2026 SonicDE contributors
# SPDX-License-Identifier: LGPL-2.0-or-later
"""Run real Screenies/SonicWin capture on private X11, D-Bus and settings.

The C++ probe compares a genuinely translucent Silver titlebar over blue and
green unrelated windows, verifies PNG alpha/background exclusion, and checks
that recompositing the PNG reproduces the actual live titlebar colors.
"""

import argparse
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compositor", required=True)
    parser.add_argument("--screenies", required=True)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--xvfb", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--dbus-run-session", required=True)
    parser.add_argument("--dbus-config", required=True)
    parser.add_argument("--private-bus-child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="screenies-real-capture-") as temporary:
        home = Path(temporary)
        config = home / ".config"
        data = home / ".local/share"
        runtime = home / "runtime"
        for directory in (config / "silver", data / "applications", runtime):
            directory.mkdir(parents=True, exist_ok=True)
        runtime.chmod(0o700)
        (config / "kwinrc").write_text(
            "[Compositing]\nEnabled=true\n[org.kde.kdecoration2]\nlibrary=org.kde.silver\n"
            "[Plugins]\nscreenshotEnabled=true\nscaleEnabled=false\nfadeEnabled=false\nblurEnabled=false\n"
        )
        (config / "silver/silverrc").write_text(
            "[Windeco]\nAnimationsEnabled=false\nMatchTitleBarToApplicationColor=false\nUseTitleBarColorForAllBorders=true\n"
            "[TitleBarOpacity]\nActiveTitleBarOpacity=50\nInactiveTitleBarOpacity=50\nOverrideActiveTitleBarOpacity=true\n"
            "OverrideInactiveTitleBarOpacity=true\nOpaqueMaximizedTitleBars=false\nBlurTransparentTitleBars=false\nApplyOpacityToHeader=false\n"
        )
        print("Private Silver config:", config / "silver/silverrc", flush=True)
        executable = str(Path(args.screenies).resolve())
        (data / "applications/org.kde.spectacle.desktop").write_text(
            f'[Desktop Entry]\nType=Application\nName=Screenies capture test\nExec="{executable}"\n'
            "X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n"
        )
        # Keep built-library discovery, not the user's live desktop/session
        # settings or platform-theme hooks. The decoration itself is real Silver.
        env = {key: os.environ[key] for key in ("PATH", "LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH", "XDG_DATA_DIRS", "LANG")
               if key in os.environ}
        env.update(HOME=str(home), XDG_CONFIG_HOME=str(config), XDG_DATA_HOME=str(data),
                   XDG_CACHE_HOME=str(home / ".cache"), XDG_STATE_HOME=str(home / ".local/state"), XDG_RUNTIME_DIR=str(runtime),
                   XDG_CONFIG_DIRS="/etc/xdg", QT_QPA_PLATFORM="xcb", QT_QPA_PLATFORMTHEME="generic", XDG_SESSION_TYPE="x11", QT_SCALE_FACTOR="1",
                   QT_QUICK_BACKEND="software", QT_X11_NO_MITSHM="1", LIBGL_ALWAYS_SOFTWARE="1", GALLIUM_DRIVER="llvmpipe",
                   SCREENIES_REAL_COMPOSITOR_TEST="1", DBUS_SYSTEM_BUS_ADDRESS="unix:path=/nonexistent/screenies-test-system",
                   PULSE_SERVER="unix:/nonexistent/screenies-test")
        if args.private_bus_child:
            env["DBUS_SESSION_BUS_ADDRESS"] = os.environ.get("DBUS_SESSION_BUS_ADDRESS", "")
        # Never inherit a route to the real desktop display or its X cookie.
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("XAUTHORITY", None)
        if not args.private_bus_child:
            # Start the bus with private HOME/XDG settings too, so activated
            # services cannot inherit the real user's configuration/display.
            return subprocess.run([args.dbus_run_session, "--config-file=" + args.dbus_config, "--", sys.executable, str(Path(__file__).resolve()),
                                   "--private-bus-child", *sys.argv[1:]], env=env).returncode
        if not env.get("DBUS_SESSION_BUS_ADDRESS"):
            raise RuntimeError("Private D-Bus session was not established")
        read_fd, write_fd = os.pipe()
        display = subprocess.Popen([args.xvfb, "-displayfd", str(write_fd), "-screen", "0", "1024x768x24", "-ac", "-nolisten", "tcp"],
                                   env=env, pass_fds=[write_fd])
        os.close(write_fd)
        compositor = None
        try:
            if not select.select([read_fd], [], [], 10)[0]:
                raise RuntimeError("Private X server did not become ready")
            number = os.read(read_fd, 64).decode().strip()
            if not number.isdecimal():
                raise RuntimeError("Private X server did not supply a display number")
            env["DISPLAY"] = ":" + number
            # No --replace, even though this is a newly allocated private X server.
            compositor = subprocess.Popen([args.compositor, "--no-kactivities"], env=env)
            result = subprocess.run([args.probe, str(output)], env=env, timeout=40)
            mappings = Path(f"/proc/{compositor.pid}/maps")
            if mappings.exists():
                libraries = {line.split()[-1] for line in mappings.read_text().splitlines()
                             if "org.kde.silver" in line or "libbreezecommon" in line}
                print("Decoration libraries:", sorted(libraries), flush=True)
                print("Silver config after capture:", (config / "silver/silverrc").read_text(), flush=True)
            return result.returncode
        finally:
            os.close(read_fd)
            if compositor is not None and compositor.poll() is None:
                # This test is about capture, not WM shutdown. Avoid restart
                # handlers spawning an untracked compositor during cleanup.
                compositor.kill()
                compositor.wait()
            display.terminate()
            display.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
