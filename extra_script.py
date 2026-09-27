import shutil, os, sys, re

Import("env")

ENV_NAME = env.subst("$PIOENV")
PROJECT_DIR = env.subst("$PROJECT_DIR")



# --- copy display panel custom board config into the ESP32_Display_Panel component ---
if ENV_NAME == "esp32-s3":
    project_conf = "esp_panel_board_custom_conf.h"

    targets = [
        os.path.join(PROJECT_DIR, "managed_components", "espressif__esp32_display_panel"),
        os.path.join(PROJECT_DIR, "managed_components", "espressif__esp32_display_panel", "src", "board"),
        os.path.join(PROJECT_DIR, "managed_components", "espressif__esp32_display_panel", "src", "board", "custom"),
    ]
    for t in targets:
        try:
            dest = os.path.join(t, "esp_panel_board_custom_conf.h")
            shutil.copy2(project_conf, dest)
            print(f"Copied {project_conf} -> {dest}")
        except OSError as e:
            print(f"skip copy {t}: {e}")

# --- force gnu++17 for all compile steps ---
for var in ['CCFLAGS', 'CXXFLAGS', 'CFLAGS']:
    flags = env.get(var, [])
    new_flags = []
    for flag in flags:
        if isinstance(flag, str) and flag.startswith('-std='):
            new_flags.append('-std=gnu++17')
        else:
            new_flags.append(flag)
    env.Replace(**{var: new_flags})

env.AppendUnique(CXXFLAGS=['-std=gnu++17'])

# Component exclusions and the PIOENV define are passed via
# `board_build.cmake_extra_args` in platformio.ini (extra_scripts run too
# late to influence the CMake configure step).

# --- WebUI inline-JS sanity check (pure stdlib, ms) ---
# A single unbalanced brace in src/WebUI.cpp kills the WHOLE page script
# (menu, status, lists) with only a console SyntaxError as a trace - exactly
# what happened with a dropped "});". Fail the build instead of shipping it.
def _js_balance_ok(src):
    stack = []
    pairs = {')': '(', ']': '[', '}': '{'}
    i, n, line = 0, len(src), 1
    prev_sig = ''
    in_s = None
    while i < n:
        c = src[i]
        if c == '\n':
            line += 1
            i += 1
            continue
        if in_s:
            if c == '\\':
                i += 2
                continue
            if c == in_s:
                in_s = None
            i += 1
            continue
        if c in ('"', "'", '`'):
            in_s = c
            i += 1
            continue
        if c == '/' and i + 1 < n:
            d = src[i + 1]
            if d == '/':
                j = src.find('\n', i)
                i = n if j < 0 else j
                continue
            if d == '*':
                j = src.find('*/', i + 2)
                if j < 0:
                    return False, 'unclosed comment at line %d' % line
                line += src.count('\n', i, j)
                i = j + 2
                continue
            if prev_sig in '=(:,[!&|?{};':
                i += 1
                in_c = False
                while i < n:
                    ch = src[i]
                    if ch == '\\':
                        i += 2
                        continue
                    if ch == '[':
                        in_c = True
                    elif ch == ']':
                        in_c = False
                    elif ch == '/' and not in_c:
                        break
                    elif ch == '\n':
                        return False, 'unterminated regex at line %d' % line
                    i += 1
                i += 1
                while i < n and src[i].isalpha():
                    i += 1
                prev_sig = 'x'
                continue
        if c in '([{':
            stack.append((c, line))
            prev_sig = c
        elif c in ')]}':
            if not stack or stack[-1][0] != pairs[c]:
                return False, 'unbalanced %r at line %d' % (c, line)
            stack.pop()
            prev_sig = c
        elif not c.isspace():
            prev_sig = c
        i += 1
    if in_s:
        return False, 'unterminated string'
    if stack:
        return False, 'unclosed %r opened at line %d' % (stack[-1][0], stack[-1][1])
    return True, 'ok'

if ENV_NAME == "esp32-s3":
    try:
        with open(os.path.join(PROJECT_DIR, "src", "WebUI.cpp"), encoding="utf-8") as f:
            _webui = f.read()
        _m = _webui.split("<script>", 1)
        _js = _m[1].rsplit("</script>", 1)[0] if len(_m) > 1 else ""
        _ok, _msg = _js_balance_ok(_js)
        if not _ok:
            print(f"WebUI JS syntax check FAILED: {_msg}")
            sys.exit(1)
        print("WebUI JS syntax check passed")
    except SystemExit:
        raise
    except Exception as e:
        print(f"WebUI JS check skipped ({e})")

# --- /dashboard mirror inline-JS sanity check (same gate as WebUI) ---
# The mirror page lives in src/WebServer.cpp as C string fragments, so the
# WebUI.cpp check never saw it - a stray "}" once shipped a dead /dashboard
# (console: "Missing catch or finally after try"). Rejoin the served page
# from the handler's fragments and balance-check its <script> blocks.
def _c_str_contents(src):
    return "\n".join(m.group(1) for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', src))

if ENV_NAME == "esp32-s3":
    try:
        with open(os.path.join(PROJECT_DIR, "src", "WebServer.cpp"), encoding="utf-8") as f:
            _ws = f.read()
        _start = _ws.index("static esp_err_t handleDashboardPage")
        _nxt = re.search(r"^static esp_err_t handle\w+", _ws[_start + 10:], re.M)
        _region = _ws[_start:_start + 10 + _nxt.start()] if _nxt else _ws[_start:]
        _page = _c_str_contents(_region)
        _parts = _page.split("<script>")
        _scripts = [p.rsplit("</script>", 1)[0] for p in _parts[1:]] if len(_parts) > 1 else []
        if not _scripts:
            print("Dashboard JS check skipped (no <script> found)")
        else:
            for _i, _js2 in enumerate(_scripts):
                _ok2, _msg2 = _js_balance_ok(_js2)
                if not _ok2:
                    print(f"Dashboard JS syntax check FAILED (block {_i}): {_msg2}")
                    sys.exit(1)
            print("Dashboard JS syntax check passed")
    except SystemExit:
        raise
    except Exception as e:
        print(f"Dashboard JS check skipped ({e})")

# --- auto-drop a fresh OTA image into dist/ after every app build ---
if ENV_NAME == "esp32-s3":
    def copy_ota_to_dist(source, target, env):
        fw = os.path.join(PROJECT_DIR, ".pio", "build", "esp32-s3", "firmware.bin")
        dst = os.path.join(PROJECT_DIR, "dist", "ota-esp32s3-8mb.bin")
        try:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(fw, dst)
            print(f"OTA image refreshed -> {dst}")
        except OSError as e:
            print(f"OTA copy skipped: {e}")

    env.AddPostAction("$BUILD_DIR/firmware.bin", copy_ota_to_dist)