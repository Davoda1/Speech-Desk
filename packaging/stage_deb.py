#!/usr/bin/env python3
"""Stage a Debian tree from the portable runtime, with a real /usr/bin executable."""
import argparse
from pathlib import Path
import re
import shutil

source = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--portable', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
version = re.search(r'project\(SpeechDesk VERSION ([0-9.]+)', (source/'CMakeLists.txt').read_text()).group(1)
out = args.output
binary = out/'usr/bin/speechdesk'
binary.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(args.portable/'speechdesk', binary)
binary.chmod(0o755)
runtime = out/'usr/lib/speechdesk'
for name in ('lib', 'plugins', 'resources', 'licenses'):
    shutil.copytree(args.portable/name, runtime/name, dirs_exist_ok=True, ignore=shutil.ignore_patterns('.*'))
shutil.copy2(args.portable/'runtime-manifest.json', runtime/'runtime-manifest.json')
for name in ('native-assets.json', 'optional-models.json', 'native-build-manifest.json'):
    if (args.portable/name).is_file():
        shutil.copy2(args.portable/name, runtime/name)
desktop = out/'usr/share/applications/speechdesk.desktop'
desktop.parent.mkdir(parents=True, exist_ok=True)
desktop.write_text('''[Desktop Entry]
Type=Application
Name=SpeechDesk
Comment=Text to speech with integrated local dictation
Exec=speechdesk
Icon=speechdesk
Terminal=false
Categories=AudioVideo;Audio;Utility;
StartupWMClass=SpeechDesk
''')
for filename, folder in [('app.svg', 'scalable'), ('app.png', '256x256')]:
    icon = out/f'usr/share/icons/hicolor/{folder}/apps'/('speechdesk'+Path(filename).suffix)
    icon.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source/'resources'/filename, icon)
control = out/'DEBIAN/control'
control.parent.mkdir(parents=True, exist_ok=True)
control.write_text(f'''Package: speechdesk
Version: {version}
Section: sound
Priority: optional
Architecture: amd64
Maintainer: davoda1
Installed-Size: 0
Depends: libc6 (>= 2.34), libstdc++6 (>= 11), libgcc-s1, libgomp1, libasound2 | libasound2t64, libpulse0, libfontconfig1, libfreetype6, libglib2.0-0 | libglib2.0-0t64, libdbus-1-3, libgl1, libegl1, libopengl0, libx11-6, libx11-xcb1, libxext6, libxrender1, libxrandr2, libxcb1, libxcb-cursor0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-render0, libxcb-shape0, libxcb-shm0, libxcb-sync1, libxcb-util1, libxcb-xfixes0, libxcb-xkb1, libxkbcommon0, libxkbcommon-x11-0, libssl3 | libssl3t64, zlib1g
Description: Text to speech with integrated offline dictation
 SpeechDesk reads text through a configured speech server with playback
 highlighting. Type, paste, load TXT or dictate into one editor. Whistle and
 Whisper Tiny, Base and Small are bundled. Optional models import separately.
 Qt 6.8.3 and FFmpeg multimedia runtimes are included.
''')
print(str(out.resolve()))
