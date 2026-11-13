#!/usr/bin/env python3
"""Extracts the GLES3 shader blobs from the game's data (assets/bin/Data of
the APK) for tools/shader_test.c, and prints statistics about them.

    pip install UnityPy lz4
    python3 tools/extract_shaders.py <extracted APK>/assets/bin/Data shaders/
"""
import sys, os, UnityPy, lz4.block, re, collections
env = UnityPy.load(sys.argv[1]); out=sys.argv[2]; os.makedirs(out,exist_ok=True)
stats=collections.Counter(); n=0
for obj in env.objects:
    if obj.type.name!="Shader": continue
    try: t=obj.read_typetree()
    except Exception as e: print("ERR",obj.path_id,e); continue
    name=t.get('m_ParsedForm',{}).get('m_Name',str(obj.path_id))
    blob=bytes(t['compressedBlob'])
    offs=t['offsets']; clen=t['compressedLengths']; dlen=t['decompressedLengths']
    plats=t['platforms']
    for pi,p in enumerate(plats):
        for seg in range(len(offs[pi])):
            o=offs[pi][seg]; c=clen[pi][seg]; d=dlen[pi][seg]
            o = o if isinstance(o,int) else o[0] if isinstance(o,list) else o
            try:
                raw=lz4.block.decompress(blob[o:o+c],uncompressed_size=d)
            except Exception as e:
                print("lz4",name,e); continue
            txt=raw.decode('latin1')
            srcs=re.findall(r'#version 3\d\d es.*?(?=\x00)',txt,re.S)
            for s in srcs:
                n+=1
                stats['progs']+=1
                for k in ['layout(std140) uniform','uniform highp sampler2DArray','sampler3D','samplerCube','texelFetch','textureLod','gl_FragDepth','floatBitsToUint','uintBitsToFloat','uvec','ivec','textureGrad','dFdx','#extension','gl_InstanceID','gl_VertexID','layout(location','UNITY_ADRENO','sampler2DShadow','textureSize','bitfield','inout','precise','unity_Builtins']:
                    if k in s: stats[k]+=1
            open(os.path.join(out,name.replace('/','_')+'.bin'),'ab').write(raw)
    print(name, plats, file=sys.stderr)
print(stats)
