#!/usr/bin/env python3
"""Exercise the packaged Reader-derived CPU worker with both bundled engines.

Uses the worker's existing JSON request/result protocol and validates the actual
PCM WAV output. Models, Python and all packages come from the staged payload.
"""
from pathlib import Path
import argparse
import json
import os
import re
import signal
import subprocess
import tempfile
import wave


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets',type=Path,required=True)
    args=parser.parse_args()
    assets=args.assets.resolve()
    python=assets/('runtime/python/python.exe' if os.name=='nt' else 'runtime/python/bin/python3')
    worker=assets/'voice_light.py'
    with tempfile.TemporaryDirectory(prefix='speechdesk-cpu-verification-') as temporary:
        work=Path(temporary)
        env={key:value for key,value in os.environ.items() if not re.match(r'^(PYTHON|HF_|TRANSFORMERS_|ORT_|ONNX_|CUDA_|ROCM_|PHONEMIZER_)',key,re.I)}
        env.update(PYTHONPATH=str(assets/'runtime/packages'),PYTHONNOUSERSITE='1',
                   PYTHONDONTWRITEBYTECODE='1',PYTHONUTF8='1',PYTHONIOENCODING='utf-8',
                   HF_HUB_OFFLINE='1',HF_HUB_DISABLE_TELEMETRY='1',ORT_DISABLE_TELEMETRY='1',
                   TRANSFORMERS_OFFLINE='1',DO_NOT_TRACK='1',CUDA_VISIBLE_DEVICES='',
                   OMP_NUM_THREADS='2',OPENBLAS_NUM_THREADS='2',MKL_NUM_THREADS='2',RAYON_NUM_THREADS='2',
                   TMPDIR=str(work),TMP=str(work),TEMP=str(work),XDG_CACHE_HOME=str(work/'cache'))
        if os.name=='nt':
            system=Path(env.get('SystemRoot','C:/Windows'))
            env['PATH']=os.pathsep.join(map(str,[python.parent,assets/'engines/pocket',system/'System32',system]))
        else:
            env['LD_LIBRARY_PATH']=str(assets/'runtime/python/lib')
            env['PATH']=os.pathsep.join([str(python.parent),'/usr/bin','/bin'])
        def run(request=None,catalog=False):
            command=[str(python),'-s','-B',str(worker)]+(['--catalog'] if catalog else [])
            child=subprocess.Popen(command,cwd=work,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                   text=True,encoding='utf-8',start_new_session=os.name!='nt')
            try:
                output,errors=child.communicate('' if request is None else json.dumps(request),timeout=600)
            except subprocess.TimeoutExpired:
                if os.name=='nt':subprocess.run(['taskkill','/PID',str(child.pid),'/T','/F'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=False)
                else:os.killpg(child.pid,signal.SIGKILL)
                child.kill();child.communicate()
                raise RuntimeError('Bundled CPU speech verification timed out')
            if child.returncode:raise RuntimeError('Bundled CPU worker failed: '+errors[-4000:]+'\n'+output[-4000:])
            return [json.loads(line) for line in output.splitlines() if line.strip()]
        catalog=run(catalog=True)[0]['engines']
        if len(catalog['pocket']['voices'])!=6 or len(catalog['kokoro']['voices'])!=28:
            raise RuntimeError('Bundled voice catalog differs from Reader presets')
        for engine,voice in [('pocket','alba'),('kokoro','af_heart')]:
            target=work/(engine+'.wav')
            request={'engine':engine,'voice':voice,'text':'Welcome to SpeechDesk. This speech is generated on this computer.',
                     'threads':2,'stream':False,'model_root':str(assets/'models'),'engine_root':str(assets/'engines'),
                     'cache_root':str(work/'cache'),'output_path':str(target)}
            events=run(request)
            result=next((event for event in events if event.get('type')=='result'),{})
            if not result.get('ok') or not target.is_file():raise RuntimeError('Bundled speech produced no completed WAV: '+engine)
            with wave.open(str(target),'rb') as audio:
                if audio.getnchannels()!=1 or audio.getsampwidth()!=2 or audio.getframerate()!=24000 or audio.getnframes()<2400:
                    raise RuntimeError('Bundled speech returned invalid PCM audio: '+engine)
                if not any(audio.readframes(audio.getnframes())):raise RuntimeError('Bundled speech returned silence: '+engine)
            print(engine+': real offline CPU synthesis passed',flush=True)

if __name__=='__main__':main()
