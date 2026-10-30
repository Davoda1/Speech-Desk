#!/usr/bin/env python3
"""Stage the real native Linux executable and its redistributable Qt runtime."""
import argparse,json,os,re,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,required=True);p.add_argument('--qt',type=Path,required=True);p.add_argument('--assets',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--sodium-manifest',type=Path);args=p.parse_args()
src=Path(__file__).resolve().parents[1];out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
for name in ('lib','plugins','resources/bin','resources/models','licenses'): (out/name).mkdir(parents=True,exist_ok=True)
shutil.copy2(args.binary,out/'speechdesk');os.chmod(out/'speechdesk',0o755)
models=['whistle.cact','ggml-tiny-q5_1.bin','ggml-base-q5_1.bin','ggml-small-q5_1.bin']
for name in models:shutil.copy2(args.assets/'models'/name,out/'resources/models'/name)
for f in (args.assets/'engines/linux').iterdir():
 if f.is_file() and not f.name.startswith('.'):shutil.copy2(f,out/'resources/bin'/f.name);os.chmod(out/'resources/bin'/f.name,0o755)
for f in (src/'vendor/licenses').iterdir():
 if f.is_file() and not f.name.startswith('.'):shutil.copy2(f,out/'licenses'/f.name)
plugins={'platforms':['libqxcb.so','libqoffscreen.so','libqminimal.so'],'multimedia':['libffmpegmediaplugin.so'],'iconengines':['libqsvgicon.so'],'imageformats':['libqsvg.so','libqjpeg.so','libqgif.so','libqico.so'],'tls':['libqopensslbackend.so','libqcertonlybackend.so']}
seeds=[args.binary.resolve()]
for directory,names in plugins.items():
 for name in names:
  f=args.qt/'plugins'/directory/name
  if f.exists():
   target=out/'plugins'/directory/name;target.parent.mkdir(exist_ok=True);shutil.copy2(f,target);seeds.append(f)
env=os.environ.copy();env['LD_LIBRARY_PATH']=str((args.qt/'lib').resolve())
seen=set();copied={};system=set()
while seeds:
 f=seeds.pop()
 if f.resolve() in seen:continue
 seen.add(f.resolve())
 report=subprocess.check_output(['ldd',str(f)],env=env,text=True)
 for line in report.splitlines():
  if 'not found' in line:raise RuntimeError(f'{f}: {line}')
  m=re.match(r'\s*(\S+)\s+=>\s+(/\S+)',line)
  if not m:continue
  soname,path=m.groups();dependency=Path(path)
  if (args.qt/'lib').resolve() in dependency.resolve().parents:
   if soname not in copied:
    shutil.copy2(dependency.resolve(),out/'lib'/soname);copied[soname]=str(dependency.resolve());seeds.append(dependency)
  else:system.add(soname)
# FFmpeg stubs may be dlopened rather than appear in the linker dependency graph.
for f in (args.qt/'lib').glob('libQt6FFmpegStub-*.so.6'):
 if f.name not in copied:shutil.copy2(f.resolve(),out/'lib'/f.name);copied[f.name]=str(f.resolve())
subprocess.run(['patchelf','--set-rpath','$ORIGIN/lib:$ORIGIN/../lib/speechdesk/lib',str(out/'speechdesk')],check=True)
for f in (out/'lib').glob('lib*.so*'):subprocess.run(['patchelf','--set-rpath','$ORIGIN',str(f)],check=True)
for f in (out/'plugins').rglob('*.so'):subprocess.run(['patchelf','--set-rpath','$ORIGIN/../../lib',str(f)],check=True)
(out/'qt.conf').write_text('[Paths]\nPrefix=.\nLibraries=lib\nPlugins=plugins\n')
manifest={'qt_version':'6.8.3','copied_libraries':sorted(copied),'system_libraries':sorted(system),'plugins':plugins}
sodium_receipt=args.sodium_manifest or args.binary.parent/'deps/sodium/build-manifest.json'
manifest['static_dependencies']={'libsodium':json.loads(sodium_receipt.read_text(encoding='utf-8'))}
if (args.assets/'build-manifest.json').is_file():shutil.copy2(args.assets/'build-manifest.json',out/'native-build-manifest.json')
(out/'runtime-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'staged':str(out),'qt_libraries':len(copied),'system_libraries':sorted(system)},indent=2))
