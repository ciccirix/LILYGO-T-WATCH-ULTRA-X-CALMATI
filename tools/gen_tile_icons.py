#!/usr/bin/env python3
# Genera le icone tile "Glyph Neon" (glyph accent + glow) come immagini LVGL
# ARGB8888 → src/tile_icons.cpp/.h, + un preview PNG per revisione.
# PIL only. Disegno in hi-res (x5) e downscale LANCZOS per anti-alias.
import math
from PIL import Image, ImageDraw, ImageFilter

S = 5                      # supersample
SZ = 84                    # final icon size (px)
HS = SZ * S                # hi-res canvas
def C(hex): return tuple(int(hex[i:i+2],16) for i in (1,3,5))
G=C('#00ff9c'); B=C('#33ccff'); A=C('#00e0ff'); P=C('#ff66cc'); Y=C('#ffaa00'); V=C('#cc99ff')
DARK=(11,15,20)            # eye punch-out colour (tile bg)

def sc(*xs): return tuple(int(round(x*HS/64.0)) for x in xs)   # viewBox64 -> hi-res
def W(w): return max(1,int(round(w*HS/64.0)))                  # stroke width

def new(): return Image.new('RGBA',(HS,HS),(0,0,0,0))
def dr(im): return ImageDraw.Draw(im)

def circle(d,cx,cy,r,fill=None,outline=None,width=1):
    x0,y0,x1,y1=sc(cx-r,cy-r,cx+r,cy+r)
    d.ellipse([x0,y0,x1,y1],fill=fill,outline=outline,width=W(width) if outline else 1)
def line(d,x0,y0,x1,y1,col,w):
    a,b,c,e=sc(x0,y0,x1,y1); d.line([a,b,c,e],fill=col,width=W(w));
    r=W(w)//2
    for (px,py) in ((a,b),(c,e)): d.ellipse([px-r,py-r,px+r,py+r],fill=col)  # round caps
def poly(d,pts,fill):
    d.polygon([sc(*p)[0:2] if False else (sc(p[0],p[1])) for p in pts],fill=fill)
def arc(d,cx,cy,r,a0,a1,col,w):
    x0,y0,x1,y1=sc(cx-r,cy-r,cx+r,cy+r); d.arc([x0,y0,x1,y1],a0,a1,fill=col,width=W(w))

# ---- le 8 icone (geometria = Set A del preview) ----
def ic_ring():   # Fai Suonare (speaker + onde)
    im=new(); d=dr(im)
    d.polygon([sc(12,26),sc(20,26),sc(30,18),sc(30,46),sc(20,38),sc(12,38)],fill=G+(255,))
    arc(d,40,32,14,-90,90,G+(255,),4)
    arc(d,46,32,24,-90,90,G+(150,),4)
    return im,G
def ic_drone():
    im=new(); d=dr(im)
    circle(d,32,32,6,fill=B+(255,))
    for cx,cy in ((16,16),(48,16),(16,48),(48,48)): circle(d,cx,cy,6,outline=B+(255,),width=4)
    for x0,y0,x1,y1 in ((21,21,27,27),(43,21,37,27),(21,43,27,37),(43,43,37,37)): line(d,x0,y0,x1,y1,B+(255,),3)
    return im,B
def ic_gatt():
    im=new(); d=dr(im)
    circle(d,22,24,11,outline=V+(255,),width=5); circle(d,22,24,3,fill=V+(255,))
    line(d,29,31,50,52,V+(255,),5); line(d,44,50,50,44,V+(255,),5); line(d,38,44,44,38,V+(255,),5)
    return im,V
def ic_replay():
    im=new(); d=dr(im)
    arc(d,32,30,16,180,300,Y+(255,),4); arc(d,32,34,16,0,120,Y+(255,),4)
    d.polygon([sc(46,12),sc(49,23),sc(38,21)],fill=Y+(255,))
    d.polygon([sc(18,52),sc(15,41),sc(26,43)],fill=Y+(255,))
    return im,Y
def ic_phantom():
    im=new(); d=dr(im)
    d.polygon([sc(16,46),sc(16,28)]+[sc(*p) for p in _ghosttop()]+[sc(48,46),sc(43,42),sc(38,46),sc(32,42),sc(26,46),sc(20,42)],fill=P+(255,))
    circle(d,26,30,3.4,fill=DARK+(255,)); circle(d,38,30,3.4,fill=DARK+(255,))
    return im,P
def _ghosttop():
    pts=[];
    for a in range(180,361,10):
        pts.append((32+16*math.cos(math.radians(a)),28+16*math.sin(math.radians(a))))
    return pts
def ic_rid():
    im=new(); d=dr(im)
    circle(d,24,42,6,fill=B+(255,)); circle(d,12,32,5,outline=B+(255,),width=3); circle(d,36,32,5,outline=B+(255,),width=3)
    arc(d,50,30,12,180,360,Y+(255,),3); arc(d,50,30,6,180,360,Y+(255,),3); circle(d,50,30,2.6,fill=Y+(255,))
    return im,B
def ic_sentinel():
    im=new(); d=dr(im)
    shield=[sc(32,8),sc(52,16),sc(52,32)]
    # shield outline as polygon path approximation
    pts=[(32,8),(52,16),(52,32),(32,56),(12,32),(12,16)]
    d.polygon([sc(*p) for p in pts],fill=G+(30,),outline=G+(255,));
    # thicken outline
    for i in range(len(pts)):
        x0,y0=pts[i]; x1,y1=pts[(i+1)%len(pts)]; line(d,x0,y0,x1,y1,G+(255,),4)
    line(d,23,32,29,38,G+(255,),4); line(d,29,38,41,24,G+(255,),4)
    return im,G
def ic_skimmer():
    im=new(); d=dr(im)
    x0,y0,x1,y1=sc(10,16,54,46); d.rounded_rectangle([x0,y0,x1,y1],radius=W(4),outline=G+(255,),width=W(4))
    x0,y0,x1,y1=sc(10,23,54,29); d.rectangle([x0,y0,x1,y1],fill=G+(255,))
    line(d,40,42,49,33,B+(255,),3); line(d,40,33,49,42,B+(255,),3); line(d,49,30,49,45,B+(255,),3)
    return im,G

R=C('#ff4d5e')

def ic_arp():     # ARP MitM: due nodi + intercettore centrale (rosso)
    im=new(); d=dr(im)
    circle(d,12,44,6,outline=B+(255,),width=4); circle(d,52,44,6,outline=B+(255,),width=4)
    line(d,18,44,46,44,B+(160,),3)                       # link vittima-gateway
    circle(d,32,20,8,fill=R+(255,))                      # attaccante (MITM)
    line(d,32,28,32,40,R+(255,),3)                       # deviazione al link
    line(d,29,36,32,40,R+(255,),3); line(d,35,36,32,40,R+(255,),3)  # freccia giù
    return im,R
def ic_bleaudit():# BLE Audit: lente + rune bluetooth (ciano)
    im=new(); d=dr(im)
    circle(d,28,28,15,outline=B+(255,),width=5); line(d,39,39,52,52,B+(255,),5)  # lente
    line(d,28,19,28,37,B+(255,),3)                       # spina BT
    line(d,28,19,34,24,B+(255,),3); line(d,34,24,28,29,B+(255,),3)
    line(d,28,27,34,32,B+(255,),3); line(d,34,32,28,37,B+(255,),3)
    line(d,22,24,34,32,B+(255,),3); line(d,22,32,34,24,B+(255,),3)
    return im,B
def ic_mifare():  # Mifare: carta contactless (ambra)
    im=new(); d=dr(im)
    x0,y0,x1,y1=sc(10,18,42,46); d.rounded_rectangle([x0,y0,x1,y1],radius=W(4),outline=Y+(255,),width=W(4))
    circle(d,18,26,2.5,fill=Y+(255,))
    for r in (7,12,17): arc(d,40,32,r,-60,60,Y+(255,),3)  # onde NFC
    return im,Y
def ic_task():    # Task Mgr: barre attività (verde)
    im=new(); d=dr(im)
    xs=[14,26,38,50]; hs=[18,34,24,42]
    for x,h in zip(xs,hs):
        x0,y0,x1,y1=sc(x-4,52-h,x+4,52); d.rounded_rectangle([x0,y0,x1,y1],radius=W(2),fill=G+(255,))
    line(d,10,52,56,52,G+(160,),2)
    return im,G
def ic_duress():  # Duress: lucchetto chiuso (rosso)
    im=new(); d=dr(im)
    arc(d,32,26,10,180,360,R+(255,),5)                  # arco (shackle)
    line(d,22,26,22,32,R+(255,),5); line(d,42,26,42,32,R+(255,),5)
    x0,y0,x1,y1=sc(16,32,48,52); d.rounded_rectangle([x0,y0,x1,y1],radius=W(4),fill=R+(255,))
    circle(d,32,40,3.2,fill=DARK+(255,));
    x0,y0,x1,y1=sc(30.6,40,33.4,48); d.rectangle([x0,y0,x1,y1],fill=DARK+(255,))  # buco chiave
    return im,R

def ic_printer(): # stampante + foglio "CALMATI" (verde)
    im=new(); d=dr(im)
    x0,y0,x1,y1=sc(20,12,44,24); d.rounded_rectangle([x0,y0,x1,y1],radius=W(2),outline=G+(255,),width=W(3))  # foglio in alto
    x0,y0,x1,y1=sc(12,24,52,42); d.rounded_rectangle([x0,y0,x1,y1],radius=W(4),fill=G+(255,))                # corpo
    circle(d,46,33,2.4,fill=DARK+(255,))                                                                     # LED
    x0,y0,x1,y1=sc(20,40,44,54); d.rounded_rectangle([x0,y0,x1,y1],radius=W(2),outline=G+(255,),width=W(3),fill=DARK+(255,))  # vassoio uscita
    line(d,24,46,40,46,G+(255,),2); line(d,24,50,36,50,G+(255,),2)                                           # righe stampate
    return im,G

LORA=C('#2ec95f')   # verde LoRa

def ic_lora():    # marchio LoRa: swirl a 3 bracci di onde attorno a un nucleo
    im=new(); d=dr(im)
    cx,cy=32,32
    for k in range(3):
        base=k*120
        arc(d,cx,cy,23,base+6,  base+92, LORA+(255,),5)   # braccio esterno
        arc(d,cx,cy,15,base+40, base+126,LORA+(220,),5)   # braccio interno (scia)
    circle(d,cx,cy,4.5,fill=LORA+(255,))                   # nucleo
    return im,LORA

ICONS=[('ic_ring',ic_ring),('ic_drone',ic_drone),('ic_gatt',ic_gatt),('ic_replay',ic_replay),
       ('ic_phantom',ic_phantom),('ic_rid',ic_rid),('ic_sentinel',ic_sentinel),('ic_skimmer',ic_skimmer),
       ('ic_arp',ic_arp),('ic_bleaudit',ic_bleaudit),('ic_mifare',ic_mifare),('ic_task',ic_task),('ic_duress',ic_duress),
       ('ic_printer',ic_printer),('ic_lora',ic_lora)]

def finish(im,accent):
    # glow: blur alpha, tint accent, composite behind sharp glyph
    alpha=im.split()[3]
    glow=Image.new('RGBA',im.size,accent+(0,)); glow.putalpha(alpha.filter(ImageFilter.GaussianBlur(HS*0.045)))
    out=Image.alpha_composite(glow,im)
    return out.resize((SZ,SZ),Image.LANCZOS)

def to_argb8888(im):  # LVGL ARGB8888 little-endian => bytes B,G,R,A
    px=im.load(); b=bytearray()
    for y in range(SZ):
        for x in range(SZ):
            r,g,bl,a=px[x,y]; b+=bytes((bl,g,r,a))
    return bytes(b)

# --- render + preview + C file ---
imgs=[]
for name,fn in ICONS:
    im,ac=fn(); imgs.append((name,finish(im,ac)))

# preview montage (4 col, righe quante servono)
import math as _m
rows=_m.ceil(len(imgs)/4)
pv=Image.new('RGBA',(SZ*4+5*10, SZ*rows+(rows+1)*10),(11,16,22,255))
for i,(name,im) in enumerate(imgs):
    col=i%4; row=i//4; pv.alpha_composite(im,(10+col*(SZ+10),10+row*(SZ+10)))
pv.save('tools/tile_icons_preview.png')

# C file
cpp=['#include "tile_icons.h"','']
for name,im in imgs:
    data=to_argb8888(im)
    arr=','.join(str(x) for x in data)
    cpp.append(f'static const uint8_t {name}_map[] = {{{arr}}};')
    cpp.append(f'const lv_image_dsc_t {name} = {{')
    cpp.append(f'  {{ LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_ARGB8888, 0, {SZ}, {SZ}, {SZ*4}, 0 }},')
    cpp.append(f'  {SZ*SZ*4}, {name}_map, NULL, NULL,')
    cpp.append('};')
    cpp.append('')
open('src/tile_icons.cpp','w').write('\n'.join(cpp))
h=['#pragma once','#include <lvgl.h>','']
for name,_ in ICONS: h.append(f'extern const lv_image_dsc_t {name};')
open('src/tile_icons.h','w').write('\n'.join(h)+'\n')
print("OK: preview tools/tile_icons_preview.png + src/tile_icons.{cpp,h} ("+str(len(ICONS))+" icone)")
