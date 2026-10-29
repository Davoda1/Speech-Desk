#!/usr/bin/env python3
"""Build self-contained release documentation (Markdown is a build-only dependency)."""
from pathlib import Path
import argparse,base64,html,sys,re
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--screenshots',type=Path);p.add_argument('--output',type=Path,required=True);p.add_argument('--markdown-path',type=Path);a=p.parse_args()
if a.markdown_path:sys.path.insert(0,str(a.markdown_path))
import markdown
out=a.output;out.mkdir(parents=True,exist_ok=True)
style='''*{box-sizing:border-box}body{margin:0;background:#f2f3f5;color:#202329;font:16px/1.65 system-ui,-apple-system,Segoe UI,sans-serif}main{max-width:1040px;margin:40px auto;padding:44px 54px;background:white;border:1px solid #dfe2e8;border-radius:16px}h1,h2,h3{line-height:1.25;letter-spacing:-.025em}h1{font-size:36px;margin-top:0}h2{font-size:25px;margin-top:44px;padding-top:18px;border-top:1px solid #e5e7eb}h3{font-size:19px;margin-top:28px}a{color:#2169ae}code{background:#f0f2f5;border-radius:4px;padding:2px 5px;font-size:.9em}pre{background:#202329;color:#e8edf3;border-radius:10px;padding:18px;overflow:auto}pre code{background:none;padding:0}table{border-collapse:collapse;width:100%;font-size:14px;margin:20px 0}td,th{text-align:left;padding:11px 13px;border:1px solid #e0e3e8;vertical-align:top}th{background:#f4f5f7}blockquote{margin:16px 0;border-left:3px solid #5d97cc;padding-left:18px;color:#515b68}.toc{background:#f7f8fa;padding:18px 24px;border-radius:12px}.note{color:#626b76;font-size:14px}.badge{display:inline-block;font-size:12px;letter-spacing:.08em;text-transform:uppercase;background:#e9f0f8;color:#2c6399;padding:4px 10px;border-radius:20px}img{max-width:100%;height:auto}button{font:inherit;cursor:pointer;border:1px solid #cbd1da;background:white;color:#27303b;padding:9px 16px;border-radius:8px}button.active{background:#2c77bd;color:white;border-color:#2c77bd}.tabs{display:flex;flex-wrap:wrap;gap:8px;margin:24px 0 18px}figure{margin:0;background:#e6e8ed;padding:12px;border-radius:12px}figcaption{font-size:13px;color:#5e6570;margin-top:10px}.feature-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:16px;margin:24px 0}.feature{border:1px solid #e0e3e8;border-radius:10px;padding:18px}.feature h3{margin:0 0 8px}@media(max-width:700px){main{margin:0;padding:24px 20px;border-radius:0;border:0}.feature-grid{grid-template-columns:1fr}table{display:block;overflow:auto}h1{font-size:29px}}@media print{body{background:white}main{border:0;margin:0;padding:0;max-width:none}.tabs{display:none}a{color:inherit}pre{white-space:pre-wrap}h2,h3{break-after:avoid}tr,img{break-inside:avoid}}'''
texts=[('Project overview',a.source/'README.md'),('User guide',a.source/'docs/User-Guide.md'),('Server profiles',a.source/'docs/Server-Profiles.md'),('Release notes',a.source/'docs/Release-Notes.md'),('Verification',a.source/'docs/Verification.md'),('Optional dictation models',a.source/'docs/Optional-Models.md'),('Native components',a.source/'docs/Native-Engines.md')]
section_ids={
    'README.md':'project-overview', 'User-Guide.md':'user-guide',
    'Server-Profiles.md':'server-profiles', 'Release-Notes.md':'release-notes',
    'Verification.md':'verification', 'Optional-Models.md':'optional-models',
    'Native-Engines.md':'native-engines',
}
sections=[]
for title,path in texts:
 text=path.read_text(encoding='utf-8')
 for filename,anchor in section_ids.items():
  text=text.replace('(docs/'+filename+')','(#'+anchor+')').replace('('+filename+')','(#'+anchor+')')
 def embed_image(match):
  image_path=a.source/match.group(2)
  if image_path.is_file():
   return '!['+match.group(1)+'](data:image/png;base64,'+base64.b64encode(image_path.read_bytes()).decode()+')'
  return match.group(0)
 text=re.sub(r'!\[([^]]*)\]\((docs/extras/[^)]+\.png)\)',embed_image,text)
 sections.append('<a id="'+section_ids[path.name]+'"></a>\n\n'+text)
content='\n\n'.join(sections)
rendered=markdown.markdown(content,extensions=['tables','fenced_code','toc'])
readme=f'<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SpeechDesk 1.0.0 — Complete guide</title><style>{style}</style></head><body><main><span class="badge">Complete offline guide · 29 October 2026</span>{rendered}</main></body></html>'
(out/'SpeechDesk-1.0.0-README.html').write_text(readme)
items=[('screenshot-dark.png','SpeechDesk · Dark'),('screenshot-light.png','SpeechDesk · Light')]
images=[]
if a.screenshots:
 for name,label in items:
  path=a.screenshots/name
  if path.is_file():
   encoded=base64.b64encode(path.read_bytes()).decode()
   images.append(f'<figure><img src="data:image/png;base64,{encoded}" alt="{html.escape(label)}"><figcaption>{html.escape(label)}</figcaption></figure>')
preview=f'''<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SpeechDesk 1.0.0 — Overview</title><style>{style}</style></head><body><main><span class="badge">Native C++ · Qt 6.8.3</span><h1>SpeechDesk</h1><p>Text to speech with integrated local dictation for Windows and Linux.</p><div class="feature-grid"><section class="feature"><h3>Choose a speech service</h3><p>Use OpenAI, ElevenLabs, OpenRouter or a compatible local or remote private server. Supply your own API key when required.</p></section><section class="feature"><h3>Follow the reading</h3><p>Play, pause, resume and stop speech in the application. Highlight words when the service supplies timings, or follow the active sentence. Save completed audio as WAV.</p></section><section class="feature"><h3>Dictate locally</h3><p>Type, paste, open text or dictate using the four bundled recognition models. Optional complete ZIP packs add Whisper Large v3 Turbo or Large v3.</p></section></div>{''.join(images)}<h2>Privacy</h2><p>Dictation stays on your computer. TTS sends text to your chosen service. API keys are session-only. SpeechDesk has no telemetry.</p><p class="note">This is an offline product overview, not the application. Service availability and voice quality depend on the selected provider.</p></main></body></html>'''
(out/'SpeechDesk-1.0.0-Preview.html').write_text(preview)
(out/'SpeechDesk-1.0.0-Notes.txt').write_text((a.source/'docs/Release-Notes.md').read_text()+'\n\n'+(a.source/'docs/Verification.md').read_text())
import shutil
if a.screenshots:
 for src,destination in [('screenshot-dark.png','SpeechDesk-1.0.0-Dark.png'),('screenshot-light.png','SpeechDesk-1.0.0-Light.png')]:
  if (a.screenshots/src).is_file():shutil.copy2(a.screenshots/src,out/destination)
print('Documentation created.')
