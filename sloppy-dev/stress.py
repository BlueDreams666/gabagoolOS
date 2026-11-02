import subprocess, random, time, os
os.environ['DISPLAY']=':9'
def sh(c): subprocess.run(c, shell=True)
g=subprocess.run("xdotool search --name '' getwindowgeometry %@ | grep -B1 480x800 | head -1", shell=True, capture_output=True, text=True).stdout
X,Y=[int(v) for v in g.split('Position: ')[1].split(' ')[0].split(',')]
def tap(x,y,hold=0.06): sh(f"xdotool mousemove {X+x} {Y+y} mousedown 1; sleep {hold}; xdotool mouseup 1")
def drag(x1,y1,x2,y2,steps=8,dt=0.012):
    cmd=f"xdotool mousemove {X+x1} {Y+y1} mousedown 1"
    for i in range(1,steps+1): cmd+=f" mousemove {X+x1+(x2-x1)*i//steps} {Y+y1+(y2-y1)*i//steps} sleep {dt}"
    sh(cmd+" mouseup 1")
random.seed(7)
def alive(): return subprocess.run("pgrep rockboxui",shell=True,capture_output=True).returncode==0
# scripted runs
for rep in range(3):
    tap(120,150); time.sleep(.5); tap(120,180); time.sleep(1.5)            # Music > All
    for _ in range(4): drag(240,650,240,150,6,.008); time.sleep(.2)          # flicks
    tap(240,400); time.sleep(1)                                              # play a song -> NP
    for _ in range(6): tap(396,690,.03); time.sleep(.15)                     # next fast
    drag(240,200,240,700,10); time.sleep(.3)                                 # close NP
    drag(10,400,400,400,6,.01); drag(10,400,400,400,6,.01)                   # back x2 quickly
    tap(120,450); time.sleep(.3); tap(120,300); time.sleep(.2); tap(250,330) # albums > album > play
    time.sleep(1); drag(240,200,240,700,8); tap(20,70); tap(20,70); tap(20,70)
    tap(360,300); time.sleep(.2); tap(240,300); time.sleep(.3); tap(120,400)  # artists > artist > album
    drag(30,500,300,500,5,.01); drag(30,500,300,500,5,.01); drag(30,500,300,500,5,.01)
    if not alive(): print('DIED at scripted', rep); break
# random chaos
for i in range(1500):
    r=random.random()
    if r<.55: tap(random.randint(5,475),random.randint(5,795),random.choice([.02,.06,.3]))
    elif r<.8: drag(random.randint(5,120),random.randint(100,700),random.randint(200,475),random.randint(100,700),random.randint(3,10),.01)
    else: drag(random.randint(50,430),random.randint(100,700),random.randint(50,430),random.randint(100,700),random.randint(3,10),.01)
    if i%50==0 and not alive(): print('DIED at random', i); break
print('alive', alive())
