# Tools

Build, deploy and asset scripts. PowerShell on Windows; the Python scripts are
plain CPython and run anywhere.

| Script | |
|---|---|
| `idf-env.ps1` | dot-source it to activate ESP-IDF v6.0.2 for the session |
| `build.ps1` | build → `build/tanmatsu/application.bin` |
| `deploy.ps1` | upload to AppFS over BadgeLink; `-Start` also launches it |
| `fetch-log.ps1` | pull `/int/multimesh/session.log` off the device |
| `make-icon.py` | regenerate `assets/icon32.png` (writes the PNG by hand — no image library needed) |
| `gen_fi_glyphs.py` | regenerate the Finnish glyphs in `components/mm_ui/font_mono_fi.c` |

## ESP-IDF

`idf-env.ps1` defaults to `K:\esp\v6.0.2\esp-idf` with its tools in `K:\esp\tools`.
Point it elsewhere without editing the script:

```powershell
$env:TANMATSU_IDF_PATH       = "C:\esp\v6.0.2\esp-idf"
$env:TANMATSU_IDF_TOOLS_PATH = "C:\esp\tools"
. .\tools\idf-env.ps1
idf.py --version                # expect v6.0.2
```

The SDK deliberately lives outside the repository so it is shared between
projects and never lands in git.

## BadgeLink

`tools/badgelink/` is **not** in this repository — it is third-party tooling,
reproducible from upstream releases. `deploy.ps1` and `fetch-log.ps1` fail with
"BadgeLink venv missing" until it is in place. To set it up:

1. Download `tools.zip` from a
   [badgeteam/esp32-component-badgelink](https://github.com/badgeteam/esp32-component-badgelink/releases)
   release and extract it to `tools/badgelink/`.
2. On Windows, put `libusb-1.0.dll` (the VS2022 **MS64** build from a
   [libusb/libusb](https://github.com/libusb/libusb/releases) release) next to
   `badgelink.py`.
3. Create the virtual environment the scripts expect:

   ```powershell
   py -3 -m venv tools\badgelink\.venv
   tools\badgelink\.venv\Scripts\pip install -r tools\badgelink\requirements.txt
   ```

On Linux, `install.sh` and `60-badgelink.rules` in that directory handle the
equivalent setup and the udev permissions.

The device must be in **BadgeLink mode** for any of it to work: press the violet
◇ key on the launcher home screen — the top-right icon changes from a bug to a
USB symbol. In debug mode it enumerates as COM ports instead and the tools report
"Badge not found". The two USB modes are mutually exclusive, which is also why
the app logs to a file rather than to serial.
