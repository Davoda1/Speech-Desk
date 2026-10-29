#!/usr/bin/env python3
"""Create SpeechDesk's optional model ZIPs from separately acquired, pinned weights.

Use --download for an explicit build-time download of missing pinned weights.
Default application/source packages do not include these weights. The app
imports only this documented, stored-ZIP pack format.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import urllib.request
import zipfile

source = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--models', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--download', action='store_true', help='Download missing weights from the pinned model revision')
args = parser.parse_args()
catalog = json.loads((source / 'vendor/optional-models.json').read_text())
license_text = (source / 'vendor/licenses/OpenAI-Whisper-MIT.txt').read_text()
args.output.mkdir(parents=True, exist_ok=True)

for model in catalog['models']:
    weights = args.models / model['filename']
    url = catalog['repository'] + '/resolve/' + catalog['revision'] + '/' + model['filename']
    if args.download and not weights.exists():
        weights.parent.mkdir(parents=True, exist_ok=True)
        temporary = weights.with_name(weights.name + '.partial')
        try:
            with urllib.request.urlopen(url, timeout=120) as response, temporary.open('wb') as output:
                shutil.copyfileobj(response, output, 1024 * 1024)
            if temporary.stat().st_size != model['size']:
                raise RuntimeError('Downloaded model has the wrong size: ' + model['filename'])
            with temporary.open('rb') as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != model['sha256']:
                    raise RuntimeError('Downloaded model SHA-256 mismatch: ' + model['filename'])
            temporary.replace(weights)
        finally:
            temporary.unlink(missing_ok=True)
    if weights.stat().st_size != model['size']:
        raise RuntimeError('Wrong model size: ' + weights.name)
    with weights.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != model['sha256']:
            raise RuntimeError('Wrong model SHA-256: ' + weights.name)
    manifest = {'format': 'speechdesk-model-pack', 'version': 1,
                **{k: model[k] for k in ('id', 'filename', 'size', 'sha256')}}
    readme = f'''SpeechDesk 1.0.0 optional dictation expansion
{model['name']}

Download the complete ZIP and select it in Settings > Dictation > Import model pack.
Keep the archive intact. After import, close Settings, open Dictate and select
the imported model. It adds recognized words to the main TTS editor.

The weights are multilingual and use Q5_0 quantization. They run locally on
the CPU through the application's bundled whisper.cpp engine. No GPU, Python,
internet connection or TTS server is needed for dictation. For slower computers,
turn Live text off and use Record followed by Stop recording. TTS still uses
your separately configured speech server.

Larger recognition models do not guarantee perfect text. Check names, numbers
and punctuation before sending the resulting text for speech.

Installed under the user's Speech-Desk configuration directory, inside models/.
The downloaded ZIP is not needed after a successful import. It works on Windows
and Linux, but must be imported separately on each computer.

Model bytes: {model['size']}
SHA-256: {model['sha256']}
Source: {url}
Model license: MIT; the complete notice is in LICENSE.txt.

This is a complete ZIP_STORED SpeechDesk model pack.
'''
    destination = args.output / model['archive']
    with zipfile.ZipFile(destination, 'w', compression=zipfile.ZIP_STORED, allowZip64=False) as pack:
        pack.writestr('manifest.json', json.dumps(manifest, indent=2) + '\n')
        pack.writestr('LICENSE.txt', license_text)
        pack.writestr('README.txt', readme)
        pack.write(weights, model['filename'])
    print(json.dumps({'file': destination.name, 'bytes': destination.stat().st_size}), flush=True)
