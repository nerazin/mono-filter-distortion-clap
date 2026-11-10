#!/usr/bin/env python3
"""Check sustained sample-timed automation in the actual CLAP at common rates."""
import pathlib, subprocess, tempfile
import numpy as np
root=pathlib.Path(__file__).resolve().parent
plugin=str(root/'MonoFilterDist.clap'); host=str(root/'clap_render')
def render(data, rate, chunk, prefix):
    ip=prefix+'.in';op=prefix+'.out';data.astype('float64').tofile(ip)
    subprocess.run([host,plugin,ip,op,str(rate),str(chunk),'0'],check=True,stdout=subprocess.DEVNULL)
    return np.fromfile(op,dtype='float64').reshape(-1,2)
with tempfile.TemporaryDirectory() as d:
    for rate in (44100,48000,96000):
        n=20000;t=np.arange(n,dtype=float);rng=np.random.default_rng(901)
        x=np.zeros((n,10));x[:,:2]=rng.uniform(-.2,.2,(n,2))
        x[:,2]=63.5+63.5*np.sin(t*.0013)
        x[:,3]=63.5+63.5*np.sin(t*.0017+.4)
        x[:,4]=63.5+63.5*np.sin(t*.0011+1.0)
        x[:,5]=63.5+63.5*np.sin(t*.0009+2.0)
        x[:,6]=-.5+63.5*np.sin(t*.0019+.7)
        x[:,7]=6*np.sin(t*.0008);x[:,8]=0;x[:,9]=80+20*np.sin(t*.001)
        a=render(x,rate,17,d+'/a');b=render(x,rate,511,d+'/b')
        if not np.all(np.isfinite(a)) or np.max(np.abs(a))>8:
            raise AssertionError(f'Unstable output at {rate}')
        error=np.max(np.abs(a-b))
        if error>1e-12: raise AssertionError(f'Block dependent automation at {rate}: {error}')
        print(f'PASS {rate} Hz rapid automation: max difference {error:.2g}, peak {np.max(abs(a)):.4f}')
