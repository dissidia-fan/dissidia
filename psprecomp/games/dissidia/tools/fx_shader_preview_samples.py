#!/usr/bin/env python3
# fx_shader_preview_samples.py -- test manifest for fx_shader_preview (see fx_shader_preview.sh):
# from the shipped work/mods/fx_shaders/manifest.json, one entry per (Alterna layer, sample tick)
# with a fake texture id and age_offset_ticks = the sample tick, plus samples.txt with the FEP
# colour/alpha each sample gets (Alterna lab spec values: forge/noctis/alterna_lab/alterna_lab_spec.json,
# FEP alpha = envelope x the peak the v8 FEP keys use). Adds a broken shader (must fall back) and a
# screen-copy shader (refraction test).
#   fx_shader_preview_samples.py <shipped manifest> <out manifest> <out samples.txt>
import json, sys, re
src, out_manifest, out_samples = sys.argv[1:4]
m = json.load(open(src))
def hx(h): return [int(h[1:3],16)/255, int(h[3:5],16)/255, int(h[5:7],16)/255]
RAMP=[(0,'#bfe6ff'),(0.42,'#c4a6ff'),(0.68,'#ff7ad6'),(1,'#ff2a40')]
def ramp(t):
    u=min(1,max(0,(t-65)/(207-65)))
    for i in range(3):
        p0,c0=RAMP[i]; p1,c1=RAMP[i+1]
        if u<=p1:
            k=(u-p0)/max(1e-4,p1-p0); a,b=hx(c0),hx(c1); return [a[j]+(b[j]-a[j])*k for j in range(3)]
    return hx(RAMP[-1][1])
# layer: (t0,t1,fin,fout,op,color or None=ramp, kind)
# v8c: the bubble is a camera-facing disc with peak alpha 70/128
L={'bubble':(0,48,2,6,.55,'#e2f3ff','quad'),'glassbreak':(64,214,2,8,.55,None,'sphere'),
   'sphere':(65,219,4,10,.8,None,'sphere'),'smear':(65,219,4,10,1.0,None,'quad'),
   'flare (':(201,247,10,2,1.0,'#ff3a58','quad'),'flare line':(201,247,10,2,1.0,'#6c5cff','line'),
   'nova arc':(222,252,8,4,0.86,'#b8d4ff','arc'),'nova':(222,252,8,4,1.0,'#ff3a1c','quad'),'afterglow (':(248,340,2,30,1.0,'#ffe2c8','quad'),
   'afterglow line':(248,340,2,30,1.0,'#c07cff','line'),'fire':(247,353,4,50,0.9,'#e8281a','sphere')}
draws=[]; samples=[]; hid=0x100
for d in m['draws']:
    if not d['name'].startswith('alterna/') or 'white glow' in d['name']: continue
    key=[k for k in L if d['name'][8:].startswith(k)]
    assert key, d['name']
    t0,t1,fin,fout,op,col,kind=L[key[0]]
    row=[]
    for k in range(6):
        t=t0+(k+0.5)/6*(t1-t0)
        env=min(1,(t-t0)/fin if fin else 1)*min(1,(t1-t)/fout if fout else 1)
        c=[1.0,1.0,1.0] if col else ramp(t)   # v8: fixed-colour layers have white FEP colour keys (u_color tints)
        e=dict(d); e['hash']='%016x'%hid; e['age_offset_ticks']=round(t,2); e['name']=d['name']+' t=%d'%t
        draws.append(e)
        samples.append('%x %s %.4f %.4f %.4f %.4f %d %s' % (hid, kind, c[0],c[1],c[2], env*min(op,1), t, key[0].strip(' (').replace(' ','_')))
        hid+=1
# GL-level fallback check: a broken shader and a screen-sampling shader
import os
outdir=os.path.dirname(out_manifest)
open(os.path.join(outdir,'broken.frag'),'w').write('vec4 fx_shade() { return vec4(undefined_thing, 1.0); }\n')
open(os.path.join(outdir,'screen.frag'),'w').write('// refraction test: the framebuffer copy, one tile row up, wobbled\n'
  'vec4 fx_shade() { vec2 uv = fx_screen_uv() + vec2(0.01 * sin(gl_FragCoord.y * 0.3), 160.0 / u_fx_screen_size.y);'
  ' return vec4(texture(u_fx_screen, uv).rgb * vec3(0.4, 1.0, 1.0), 1.0); }\n')
draws.append({'name':'broken','hash':'%016x'%0xbad,'frag':'broken.frag'})
draws.append({'name':'screen','hash':'%016x'%0x5c,'frag':'screen.frag','screen':True,'blend':'off'})
m['draws']=draws
json.dump(m,open(out_manifest,'w'),indent=1)
open(out_samples,'w').write('\n'.join(samples)+'\n')
print(len(draws),'entries')
