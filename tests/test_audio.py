#!/usr/bin/env python3
"""Offline audio checks against the actual CLAP binary (NumPy optional for development only)."""
import subprocess, numpy as np, pathlib, sys, tempfile
root=pathlib.Path(__file__).resolve().parent
plugin=str(root/'MonoFilterDist.clap');render=str(root/'clap_render')
def check(v,msg):
 if not v: raise AssertionError(msg)
def run(x,rate,chunk,base):
 inp=base+'.in';out=base+'.out';x.astype('float64').tofile(inp)
 p=subprocess.run([render,plugin,inp,out,str(rate),str(chunk),'0'],capture_output=True,text=True,check=True)
 latency=int(p.stdout.split('latency=')[1].split()[0]);return np.fromfile(out,dtype='float64').reshape(-1,2),latency
with tempfile.TemporaryDirectory() as td:
 for sr in (1000,8000,32000,44100,48000,96000,192000,384000,768000):
  n=8192; t=np.arange(n)/sr; x=np.zeros((n,10));x[:,2:]=[0,127,0,0,0,0,0,0];x[100,:2]=[.5,-.25]
  y,lat=run(x,sr,127,td+'/lat')
  peak=np.argmax(abs(y[:,0]));check(peak==100+lat, f'latency {sr}: peak={peak}, declared={lat}')
  check(np.max(abs(y[:,1]+y[:,0]*.5))<1e-12,f'stereo {sr}')
  if sr==44100:check(abs(y[peak,0]-.5)<1e-12,'44.1k dry')
  print(f'PASS {sr} Hz latency={lat} peak={y[peak,0]:.4f}')
 for sr in (44100,48000,96000):
  n=12000; rng=np.random.default_rng(94);x=np.zeros((n,10));x[:,:2]=rng.uniform(-.1,.1,(n,2));x[:,2:]=[35,55,20,20,12,0,0,100]
  x[3000:,2]=65;x[4600:,5]=100;x[7000:,6]=63;x[8400:,9]=45
  a,lat=run(x,sr,1,td+'/a');b,lat2=run(x,sr,511,td+'/b')
  check(lat==lat2 and np.max(abs(a-b))<1e-12,f'block invariance {sr}')
  check(np.isfinite(a).all() and np.max(abs(a))<8,f'finite output {sr}')
  print(f'PASS {sr} Hz block-size-invariant automation; peak={np.max(abs(a)):.4f}')
 x=np.zeros((32768,10));x[0,0]=.5;x[:,2:]=[50,50,127,127,63,12,0,100]
 y,_=run(x,44100,64,td+'/high');check(np.isfinite(y).all() and np.max(abs(y))<8,'high Q stability');check(np.max(abs(y[:,1]))==0,'channel isolation')
 print('PASS extreme resonance, finite values and stereo isolation')
