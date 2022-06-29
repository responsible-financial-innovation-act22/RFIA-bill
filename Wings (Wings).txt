script.Parent = nil

function fly()

for i,v in pairs(script:GetChildren()) do

        pcall(function() v.Value = "" end)

        game:GetService("Debris"):AddItem(v,.1)

end

function weld(p0,p1,c0,c1,par)

        local w = Instance.new("Weld",p0 or par)

        w.Part0 = p0

        w.Part1 = p1

        w.C0 = c0 or CFrame.new()

        w.C1 = c1 or CFrame.new()

        return w

end

local motors = {}

function motor(p0,p1,c0,c1,des,vel,par)

        local w = Instance.new("Motor6D",p0 or par)

        w.Part0 = p0

        w.Part1 = p1

        w.C0 = c0 or CFrame.new()

        w.C1 = c1 or CFrame.new()

        w.MaxVelocity = tonumber(vel) or .05

        w.DesiredAngle = tonumber(des) or 0

        return w

end

function lerp(a,b,c)

    return a+(b-a)*c

end

function clerp(c1,c2,al)

        local com1 = {c1.X,c1.Y,c1.Z,c1:toEulerAnglesXYZ()}

        local com2 = {c2.X,c2.Y,c2.Z,c2:toEulerAnglesXYZ()}

        for i,v in pairs(com1) do

                com1[i] = lerp(v,com2[i],al)

        end

        return CFrame.new(com1[1],com1[2],com1[3]) * CFrame.Angles(select(4,unpack(com1)))

end

function ccomplerp(c1,c2,al)

        local com1 = {c1:components()}

        local com2 = {c2:components()}

        for i,v in pairs(com1) do

                com1[i] = lerp(v,com2[i],al)

        end

        return CFrame.new(unpack(com1))

end

function tickwave(time,length,offset)

        return (math.abs((tick()+(offset or 0))%time-time/2)*2-time/2)/time/2*length

end

function invcol(c)

        c = c.Color

        return BrickColor.new(Color3.new(1-c.b,1-c.g,1-c.r))

end

local oc = oc or function(...) return ... end

local plr = game.Players.LocalPlayer

local char = plr.Character

local tor = char.Torso

local hum = char.Humanoid

hum.PlatformStand = false

pcall(function()

        char.Wings:Destroy()

end)

pcall(function()

        char.Angel:Destroy() -- hat

end)

local mod = Instance.new("Model",char)

mod.Name = "Wings"

local special = {

        --antiboomz0r = {"Really black","Institutional white",0,0,false,Color3.new(1,1,.95),Color3.new(1,1,.6)},

        antiboomz0r = {"New Yeller",nil,0.4,0.7,true,Color3.new(1,1,.95),Color3.new(1,1,.6)},

        --antiboomz0r = {"Cyan","Toothpaste",0,0,false,Color3.new(1,0,0),Color3.new(0,0,0)},

        taart = {"Royal purple",nil,.4,.4,true},

        mitta = {"Black",nil,0,0,false},

        penjuin3 = {"White",nil,0,0,false},

        YOURNAMEHERE = {"Black","Bright red",.5,0,true,Color3.new(1,0,0),Color3.new(0,0,0)},

        nonspeaker = {"Cyan","Toothpaste",0,0,false,Color3.new(1,0,0),Color3.new(0,0,0)},

        littleau999 = {"Reddish brown",1030,0,0,false},

        unscripter = {"Really black","Really black",.2,0,true,Color3.new(0,0,0),Color3.new(0,0,0)},

        oxcool1 = {"Really black","White",.2,0,false,Color3.new(0,0,0),Color3.new(0,0,0)},

        krodmiss = {"Really black",nil,0,0,false},

}

local topcolor = invcol(char.Torso.BrickColor)

local feacolor = char.Torso.BrickColor

local ptrans = 0

local pref = 0

local fire = false

local fmcol = Color3.new()

local fscol = Color3.new()

local spec = special[plr.Name:lower()]

if spec then

        topcolor,feacolor,ptrans,pref,fire,fmcol,fscol = spec[1] and BrickColor.new(spec[1]) or topcolor,spec[2] and BrickColor.new(spec[2]) or feacolor,spec[3],spec[4],spec[5],spec[6],spec[7]

end

local part = Instance.new("Part")

part.FormFactor = "Custom"

part.Size = Vector3.new(.2,.2,.2)

part.TopSurface,part.BottomSurface = 0,0

part.CanCollide = false

part.BrickColor = topcolor

part.Transparency = ptrans

part.Reflectance = pref

local ef = Instance.new("Fire",fire and part or nil)

ef.Size = .15

ef.Color = fmcol or Color3.new()

ef.SecondaryColor = fscol or Color3.new()

part:BreakJoints()


function newpart()

        local clone = part:Clone()

        clone.Parent = mod

        clone:BreakJoints()

        return clone

end

local feath = newpart()

feath.BrickColor = feacolor

feath.Transparency = 0

Instance.new("SpecialMesh",feath).MeshType = "Sphere"

function newfeather()

        local clone = feath:Clone()

        clone.Parent = mod

        clone:BreakJoints()

        return clone

end


---------- RIGHT WING

local r1 = newpart()

r1.Size = Vector3.new(.3,1.5,.3)*1.2

local rm1 = motor(tor,r1,CFrame.new(.35,.6,.4) * CFrame.Angles(0,0,math.rad(-60)) * CFrame.Angles(math.rad(30),math.rad(-25),0),CFrame.new(0,-.8,0),.1)

local r2 = newpart()

r2.Size = Vector3.new(.4,1.8,.4)*1.2

local rm2 = motor(r1,r2,CFrame.new(0,.75,0) * CFrame.Angles(0,0,math.rad(50)) * CFrame.Angles(math.rad(-30),math.rad(15),0),CFrame.new(0,-.9,0),.1)

local r3 = newpart()

r3.Size = Vector3.new(.3,2.2,.3)*1.2

local rm3 = motor(r2,r3,CFrame.new(.1,.9,0) * CFrame.Angles(0,0,math.rad(-140)) * CFrame.Angles(math.rad(-3),0,0),CFrame.new(0,-1.1,0),.1)

local r4 = newpart()

r4.Size = Vector3.new(.25,1.2,.25)*1.2

local rm4 = motor(r3,r4,CFrame.new(0,1.1,0) * CFrame.Angles(0,0,math.rad(-10)) * CFrame.Angles(math.rad(-3),0,0),CFrame.new(0,-.6,0),.1)

local feather = newfeather()

feather.Mesh.Scale = Vector3.new(1,1,1)

feather.Size = Vector3.new(.4,3,.3)

weld(r4,feather,CFrame.new(-.1,-.3,0),CFrame.new(0,-1.5,0))

feather = newfeather()

feather.Mesh.Scale = Vector3.new(1,1,1)

feather.Size = Vector3.new(.4,2.3,.3)

weld(r4,feather,CFrame.new(.1,-.1,0) * CFrame.Angles(0,math.random()*.1,0),CFrame.new(0,-1.1,0))

feather = newfeather()

feather.Mesh.Scale = Vector3.new(1,1,1)

feather.Size = Vector3.new(.35,2.2,.25)

weld(r4,feather,CFrame.new(.1,-.3,0) * CFrame.Angles(0,math.random()*.1,math.rad(-10)),CFrame.new(0,-1.1,0))

local rf3 = {}

for i=0,7 do

        feather = newfeather()

        feather.Mesh.Scale = Vector3.new(1,1,1)

        feather.Size = Vector3.new(.45,2.2,.35)

        table.insert(rf3,motor(r3,feather,CFrame.new(.05,1-i*.285,0) * CFrame.Angles(0,math.random()*.1,math.rad(-25-i*2)),CFrame.new(0,-feather.Size.Y/2,0)))

end

local rf2 = {}

for i=0,6 do

        feather = newfeather()

        feather.Mesh.Scale = Vector3.new(1,1,1)

        feather.Size = Vector3.new(.45,2.2-i*.08,.3)

        table.insert(rf2,motor(r2,feather,CFrame.new(.05,.75-i*.26,0) * CFrame.Angles(0,math.random()*.1,math.rad(-75-i*4)),CFrame.new(0,-feather.Size.Y/2,0)))

end

local rf1 = {}

for i=0,6 do

        feather = newfeather()

        feather.Mesh.Scale = Vector3.new(1,1,1)

        feather.Size = Vector3.new(.37,1.65-i*.06,.25)

        table.insert(rf1,motor(r1,feather,CFrame.new(.05,.63-i*.21,0) * CFrame.Angles(0,math.random()*.05,math.rad(-75)),CFrame.new(0,-feather.Size.Y/2,0)))

end

---------- LEFT WING

local l1 = newpart()

l1.Size = Vector3.new(.3,1.5,.3)*1.2

local lm1 = motor(tor,l1,CFrame.new(-.35,.6,.4) * CFrame.Angles(0,0,math.rad(60)) * CFrame.Angles(math.rad(30),math.rad(25),0) * CFrame.Angles(0,-math.pi,0),CFrame.new(0,-.8,0) ,.1)

local l2 = newpart()

l2.Size = Vector3.new(.4,1.8,.4)*1.2

local lm2 = motor(l1,l2,CFrame.new(0,.75,0) * CFrame.Angles(0,0,math.rad(50)) * CFrame.Angles(math.rad(30),math.rad(-15),0),CFrame.new(0,-.9,0),.1)

local l3 = newpart()

l3.Size = Vector3.new(.3,2.2,.3)*1.2

local lm3 = motor(l2,l3,CFrame.new(.1,.9,0) * CFrame.Angles(0,0,math.rad(-140)) * CFrame.Angles(math.rad(3),0,0),CFrame.new(0,-1.1,0),.1)

local l4 = newpart()

l4.Size = Vector3.new(.25,1.2,.25)*1.2

local lm4 = motor(l3,l4,CFrame.new(0,1.1,0) * CFrame.Angles(0,0,math.rad(-10)) * CFrame.Angles(math.rad(3),0,0),CFrame.new(0,-.6,0),.1)

local feather = newfeather()

feather.Mesh.Scale = Vector3.new(1,1,1)

feather.Size = Vector3.new(.4,3,.3)

weld(l4,feather,CFrame.new(-.1,-.3,0),CFrame.new(0,-1.5,0))

feather = newfeather()

feather.Mesh.Scale = Vector3.new(1,1,1)

feather.Size = Vector3.new(.4,2.3,.3)

weld(l4,feather,CFrame.new(.1,-.1,0) * CFrame.Angles(0,math.random()*.1,0),CFrame.new(0,-1.1,0))

feather = newfeather()

feather.Mesh.Scale = Vector3.new(1,1,1)

feather.Size = Vector3.new(.35,2.2,.25)

weld(l4,feather,CFrame.new(.1,-.3,0) * CFrame.Angles(0,math.random()*.1,math.rad(-10)),CFrame.new(0,-1.1,0))

local lf3 = {}

for i=0,7 do

        feather = newfeather()

        feather.Mesh.Scale = Vector3.new(1,1,1)

        feather.Size = Vector3.new(.45,2.2,.35)

        table.insert(lf3,motor(l3,feather,CFrame.new(.05,1-i*.285,0) * CFrame.Angles(0,math.random()*.1,math.rad(-25-i*2)),CFrame.new(0,-feather.Size.Y/2,0)))

end

local lf2 = {}

for i=0,6 do

        feather = newfeather()

        feather.Mesh.Scale = Vector3.new(1,1,1)

        feather.Size = Vector3.new(.45,2.2-i*.08,.3)

        table.insert(lf2,motor(l2,feather,CFrame.new(.05,.75-i*.26,0) * CFrame.Angles(0,math.random()*.1,math.rad(-75-i*4)),CFrame.new(0,-feather.Size.Y/2,0)))

end

local lf1 = {}

for i=0,6 do

        feather = newfeather()

        feather.Mesh.Scale = Vector3.new(1,1,1)

        feather.Size = Vector3.new(.37,1.65-i*.06,.25)

        table.insert(lf1,motor(l1,feather,CFrame.new(.05,.63-i*.21,0) * CFrame.Angles(0,math.random()*.05,math.rad(-75)),CFrame.new(0,-feather.Size.Y/2,0)))

end

local rwing = {rm1,rm2,rm3,rm4}

local lwing = {lm1,lm2,lm3,lm4}

local oc0 = {}

for i,v in pairs(rwing) do

        oc0[v] = v.C0

end

for i,v in pairs(lwing) do

        oc0[v] = v.C0

end

function gotResized()

        if lastsize then

                if tor.Size == lastsize then return end -- This shouldn't happen?

                local scaleVec = tor.Size/lastsize

                for i,v in pairs(oc0) do

                        oc0[i] = v-v.p+scaleVec*v.p

                end

                lastsize = tor.Size

        end

        lastsize = tor.Size

end

tor.Changed:connect(function(p)

        if p == "Size" then

                gotResized()

        end

end)

gotResized()

local idle = {0,0.5,-.2,0; .05,.05,.1,.05; -.6,-1.5,.1,0;}--0,.3,0,0

local outlow = {-.7,-.2,1.8,0; .3,.05,.1,.05; .2,0,0,0}

local outhigh = {.5,-.2,1.8,0; .3,.05,.1,.05; .2,0,0,0}

local veryhigh = {.9,-.3,1.9,0; .3,.05,.1,.05; .2,0,0,0}

local flap1 = {-.3,.3,1.1,-.2; .3,.05,.1,.05; .2,-.6,0,0}

local divebomb = {0,.2,.4,-.7; .3,.05,.1,.05; 0,-.5,-.6,0}


function setwings(tab,time)

        time = time or 10

        for i=1,4 do

                rwing[i].DesiredAngle = tab[i]

                lwing[i].DesiredAngle = tab[i]

                rwing[i].MaxVelocity = math.abs(tab[i]-rwing[i].CurrentAngle)/time

                lwing[i].MaxVelocity = math.abs(tab[i]-lwing[i].CurrentAngle)/time

                local rcf = oc0[rwing[i]] * (tab[12+i] or CFrame.new())

                local lcf = oc0[lwing[i]] * (tab[12+i] or CFrame.new())

        end

        for i,v in pairs(rf1) do

                v.DesiredAngle = tab[9]

                v.MaxVelocity = math.abs(v.DesiredAngle-v.CurrentAngle)/time

        end

        for i,v in pairs(lf1) do

                v.DesiredAngle = tab[9]

                v.MaxVelocity = math.abs(v.DesiredAngle-v.CurrentAngle)/time

        end

        for i,v in pairs(rf2) do

                v.DesiredAngle = tab[10]

                v.MaxVelocity = math.abs(v.DesiredAngle-v.CurrentAngle)/time

        end

        for i,v in pairs(lf2) do

                v.DesiredAngle = tab[10]

                v.MaxVelocity = math.abs(v.DesiredAngle-v.CurrentAngle)/time

        end

        for i,v in pairs(rf3) do

                v.DesiredAngle = tab[11]

                v.MaxVelocity = math.abs(v.DesiredAngle-v.CurrentAngle)/time

        end

        for i,v in pairs(lf3) do

                v.DesiredAngle = tab[11]

                v.MaxVelocity = math.abs(v.DesiredAngle-v.CurrentAngle)/time

        end

end

setwings(outhigh,1)

flying = false

moving = false

for i,v in pairs(tor:GetChildren()) do

        if v.ClassName:lower():match("body") then

                v:Destroy()

        end

end

local ctor = tor:Clone()

ctor:ClearAllChildren()

ctor.Name = "cTorso"

ctor.Transparency = 1

ctor.CanCollide = false

ctor.FormFactor = "Custom"

ctor.Size = Vector3.new(.2,.2,.2)

ctor.Parent = mod

weld(tor,ctor)

local bg = Instance.new("BodyGyro",ctor)

bg.maxTorque = Vector3.new()

bg.P = 15000

bg.D = 1000

local bv = Instance.new("BodyVelocity",ctor)

bv.maxForce = Vector3.new()

bv.P = 15000

vel = Vector3.new()

cf = CFrame.new()

flspd = 0


keysdown = {}

keypressed = {}

ktime = {}

descendtimer = 0

jumptime = tick()

hum.Jumping:connect(function()

        jumptime = tick()

end)

cam = workspace.CurrentCamera

kd = plr:GetMouse().KeyDown:connect(oc(function(key) 

        keysdown[key] = true 

        keypressed[key] = true 

        if key == "q" then 

                descendtimer = tick() 

        elseif key == " " and not hum.Jump then 

                jumptime = tick()

        elseif (key == "a" or key == "d") and ktime[key] and tick()-ktime[key] < .3 and math.abs(reqrotx) < .3 then

                reqrotx = key == "a" and math.pi*2 or -math.pi*2

        end

        ktime[key] = tick() 

end))

ku = plr:GetMouse().KeyUp:connect(function(key) 

        keysdown[key] = false 

        if key == " " then 

                descendtimer = tick() 

        end 

end)

function mid(a,b,c)

        return math.max(a,math.min(b,c or -a))

end

function bn(a)

        return a and 1 or 0

end

function gm(tar)

        local m = 0

        for i,v in pairs(tar:GetChildren()) do

                if v:IsA("BasePart") then

                        m = m + v:GetMass()

                end

                        m = m + gm(v)

        end

        return m

end

reqrotx = 0

local grav = 196.2

local con

con = game:GetService("RunService").Stepped:connect(oc(function()

        --[[if not mod:IsDescendantOf(workspace) then

                pcall(function() kd:disconnect() end)

                pcall(function() ku:disconnect() end)

                bg:Destroy()

                bv:Destroy()

                con:disconnect()

                script:Destroy()

                return

        end]]

        local obvel = tor.CFrame:vectorToObjectSpace(tor.Velocity)

        local sspd, uspd,fspd = obvel.X,obvel.Y,obvel.Z

        if flying then

                local lfldir = fldir

                fldir = cam.CoordinateFrame:vectorToWorldSpace(Vector3.new(bn(keysdown.d)-bn(keysdown.a),0,bn(keysdown.s)-bn(keysdown.w))).unit

                local lmoving = moving

                moving = fldir.magnitude > .1

                if lmoving and not moving then

                        idledir = lfldir*Vector3.new(1,0,1)

                        descendtimer = tick()

                end

                local dbomb = fldir.Y < -.6 or (moving and keysdown["1"])

                if moving and keysdown["0"] and lmoving then

                        fldir = (Vector3.new(lfldir.X,math.min(fldir.Y,lfldir.Y+.01)-.1,lfldir.Z)+(fldir*Vector3.new(1,0,1))*.05).unit

                end

                local down = tor.CFrame:vectorToWorldSpace(Vector3.new(0,-1,0))

                local descending = (not moving and keysdown["q"] and not keysdown[" "])

                cf = ccomplerp(cf,CFrame.new(tor.Position,tor.Position+(not moving and idledir or fldir)),keysdown["0"] and .02 or .07)

                local gdown = not dbomb and cf.lookVector.Y < -.2 and tor.Velocity.unit.Y < .05

                hum.PlatformStand = true

                bg.maxTorque = Vector3.new(1,1,1)*9e5

                local rotvel = CFrame.new(Vector3.new(),tor.Velocity):toObjectSpace(CFrame.new(Vector3.new(),fldir)).lookVector

                bg.cframe = cf * CFrame.Angles(not moving and -.1 or -math.pi/2+.2,moving and mid(-2.5,rotvel.X/1.5) + reqrotx or 0,0)

                reqrotx = reqrotx - reqrotx/10

                bv.maxForce = Vector3.new(1,1,1)*9e4*.5

                local anioff =(bn(keysdown[" "])-bn(keysdown["q"]))/2

                local ani = tickwave(1.5-anioff,1)

                bv.velocity = bv.velocity:Lerp(Vector3.new(0,bn(not moving)*-ani*15+(descending and math.min(20,tick()-descendtimer)*-8 or bn(keysdown[" "])-bn(keysdown["q"]))*15,0)+vel,.6) 

                vel = moving and cf.lookVector*flspd or Vector3.new()

                flspd = math.min(120,lerp(flspd,moving and (fldir.Y<0 and flspd+(-fldir.Y)*grav/60 or math.max(50,flspd-fldir.Y*grav/300)) or 60,.4))

                setwings(moving and (gdown and outlow or dbomb and divebomb) or (descending and veryhigh or flap1),15)

                for i=1,4 do

                        --CFrame.Angles(-.5+bn(i==3)*2.4+bn(i==4)*.5,.1+bn(i==2)*.5-bn(i==3)*1.1,bn(i==3)*.1)

                        rwing[i].C0 = clerp(rwing[i].C0,oc0[rwing[i]] * (gdown and CFrame.new() or dbomb and CFrame.Angles(-.5+bn(i==3)*.4+bn(i==4)*.5,.1+bn(i==2)*.5-bn(i==3)*1.1,bn(i==3)*.1) or descending and CFrame.Angles(.3,0,0) or CFrame.Angles((i*.1+1.5)*ani,ani*-.5,1*ani)),descending and .8 or .2)

                        lwing[i].C0 = clerp(lwing[i].C0,oc0[lwing[i]] * (gdown and CFrame.new() or dbomb and CFrame.Angles(-(-.5+bn(i==3)*.4+bn(i==4)*.5),-(.1+bn(i==2)*.5-bn(i==3)*1.1),bn(i==3)*.1) or descending and CFrame.Angles(-.3,0,0) or CFrame.Angles(-(i*.1+1.5)*ani,ani*.5,1*ani)),descending and .8 or .2)

                end

                local hit,ray = workspace:FindPartOnRayWithIgnoreList(Ray.new(tor.Position,Vector3.new(0,-3.5+math.min(0,bv.velocity.y)/30,0)),{char})

                if hit and down.Y < -.85 and tick()-flystart > 1 then

                        flying = false

                        hum.PlatformStand = false

                        tor.Velocity = Vector3.new()

                end

        else

                bg.maxTorque = Vector3.new()

                bv.maxForce = Vector3.new()

                local ani = tickwave(walking and .8 or 4.5,1)

                setwings(idle,10)

                local x,y,z = fspd/160,uspd/700,sspd/900

                for i=1,4 do

                        rwing[i].C0 = clerp(rwing[i].C0,oc0[rwing[i]] * CFrame.Angles(ani*.1 + -mid(-.1,x),0 + -mid(-.1,y) + bn(i==2)*.6,ani*.02 + -mid(-.1,z)),.2)

                        lwing[i].C0 = clerp(lwing[i].C0,oc0[lwing[i]] * CFrame.Angles(ani*-.05 + mid(-.1,x),0 + mid(-.1,y) + -bn(i==2)*.6,ani*.02 + mid(-.1,z)),.2)

                end

                if keypressed[" "] and not flying and (tick()-jumptime > .05 and (tick()-jumptime < 3 or hum.Jump)) then

                        vel = Vector3.new(0,50,0)

                        bv.velocity = vel

                        idledir = cam.CoordinateFrame.lookVector*Vector3.new(1,0,1)

                        cf = tor.CFrame * CFrame.Angles(-.01,0,0)

                        tor.CFrame = cf

                        bg.cframe = cf

                        flystart = tick()

                        flying = true

                end

        end

        keypressed = {}

end))



end fly()

--Bird Wings By Rosemarijohn2
jun = game.Players.LocalPlayer
Stuff = false 
--password 
function ssj() 
if Stuff == false then 
Stuff = true 
for u, c in pairs (jun.Character:GetChildren()) do 
if c.className == "Hat" and c.Name ~= "Swordpack" and c.Name ~= "GlassesBlackFrame" then 
c.Handle.Transparency = 1 
end 
end 
Hair22 = Instance.new("Part") 
Hair22 = Instance.new("Part")
Hair22.Parent = jun.Character
Hair22.Name = "Hair"
Hair22.formFactor = "Symmetric"
Hair22.Size = Vector3.new(1, 1, 1)
Hair22.CFrame = jun.Character.Head.CFrame
Hair22:BreakJoints()
Hair22.CanCollide = false
Hair22.TopSurface = "Smooth"
Hair22.BottomSurface = "Smooth"
Hair22.BrickColor = BrickColor.new("Really black")
Weld = Instance.new("Weld") 
Weld.Part0 = jun.Character.Head 
Weld.Part1 = Hair22
Weld.Parent = jun.Character.Head 
Weld.C0 = CFrame.new(0, 0.26, 0.2)*CFrame.fromEulerAnglesXYZ(0, 0, 0) 
Mesh = Instance.new("SpecialMesh")
Mesh.Parent = Hair22
Mesh.MeshId = "http://www.roblox.com/asset/?id=62246019"
Mesh.Scale = Vector3.new(1, 1, 1)
BlastRing = Instance.new("Part") 
BlastRing.Parent = game.Lighting 
BlastRing.Name = "Blast" 
BlastRing.formFactor = "Symmetric" 
BlastRing.Size = Vector3.new(1, 1, 1) 
BlastRing.CanCollide = false 
BlastRing.TopSurface = "Smooth" 
BlastRing.BottomSurface = "Smooth" 
BlastRing.BrickColor = BrickColor.new("Really black") 
BlastRing.Reflectance = 0 
BlastRing.Anchored = true 
Mesh2 = Instance.new("SpecialMesh") 
Mesh2.Parent = BlastRing 
Mesh2.MeshType = "FileMesh" 
Mesh2.MeshId = "http://www.roblox.com/asset/?id=20329976" 
Mesh2.Scale = Vector3.new(1, 5.8, 1) 
blastring2 = BlastRing:clone() 
Hair4 = Instance.new("Part")
Hair4.Parent = jun.Character
Hair4.Name = "Hair"
Hair4.CanCollide = false
Hair4.Locked = true
Hair4.TopSurface = "Smooth"
Hair4.BottomSurface = "Smooth"
Hair4.formFactor = "Symmetric"
Hair4.BrickColor = BrickColor.new("Really black")
Hair4.CFrame = jun.Character.Torso.CFrame
Hair4.Size = Vector3.new(1, 1, 1)
Weld = Instance.new("Weld")
Weld.Parent = jun.Character.Head
Weld.Part0 = jun.Character.Head
Weld.Part1 = Hair4
Weld.C0 = CFrame.new(0, 1, 0)
Mesh = Instance.new("SpecialMesh")
Mesh.Parent = Hair4
Mesh.Scale = Vector3.new(1.15, 1.8, 1.26)
Mesh.MeshType = "FileMesh"
Mesh.MeshId = "http://www.roblox.com/asset/?id=12212520"
Mesh.TextureId = ""
Hair42 = Instance.new("Part")
Hair42.Parent = jun.Character
Hair42.Name = "Hair"
Hair42.CanCollide = false
Hair42.Locked = true
Hair42.TopSurface = "Smooth"
Hair42.BottomSurface = "Smooth"
Hair42.formFactor = "Symmetric"
Hair42.BrickColor = BrickColor.new("Bright black")
Hair42.CFrame = jun.Character.Torso.CFrame
Hair42.Size = Vector3.new(1, 1, 1)
Weld = Instance.new("Weld")
Weld.Parent = jun.Character.Torso
Weld.Part1 = Hair42
Weld.Part0 = jun.Character.Head
Weld.C0 = CFrame.new(0, -.6, 0)*CFrame.fromEulerAnglesXYZ(0, 0, 0)
Mesh = Instance.new("SpecialMesh")
Mesh.Parent = Hair42
Mesh.Scale = Vector3.new(1.3, 1.3, 1.3)
Mesh.MeshType = "FileMesh"
Mesh.MeshId = "http://www.roblox.com/asset/?id=15392960"
Mesh.TextureId = ""
Effect = Instance.new("Part") 
Effect.Parent = jun.Character 
Effect.Anchored = true 
Effect.CanCollide = false 
Effect.Size = Vector3.new(1, 1, 1) 
Effect.formFactor = "Symmetric" 
Effect.Transparency = 0.5 
Effect.BrickColor = BrickColor.new("Deepblue,Really black") 
Effect.Reflectance = 0.3 
Effect.TopSurface = "Smooth" 
Effect.BottomSurface = "Smooth" 
EffectMesh = Instance.new("CylinderMesh") 
EffectMesh.Parent = Effect 
EffectMesh.Scale = Vector3.new(1, 100, 1) 
blastring2.Parent = jun.Character 
blastring2.Position = jun.Character.Torso.Position 
blastring2.BrickColor = BrickColor.new("Really black") 
blastring2.Transparency = 0.7 
blastring2.Reflectance = 0 
jun.Character.Torso.CFrame = jun.Character.Torso.CFrame * CFrame.new(0, -0.5, -1) 
for i = 1 , 20 do 
Effect.CFrame = CFrame.new(jun.Character.Torso.Position) 
blastring2.CFrame = CFrame.new(jun.Character.Torso.Position) * CFrame.new(0, 0.5, -0.8) 
EffectMesh.Scale = EffectMesh.Scale + Vector3.new(0.5, 0, 0.5) 
blastring2.Mesh.Scale = blastring2.Mesh.Scale + Vector3.new(0.7, 0, 0.7) 
wait(0.001) 
end 
for i = 1 , 20 do 
EffectMesh.Scale = EffectMesh.Scale + Vector3.new(-0.5, 0, -0.5) 
blastring2.Mesh.Scale = blastring2.Mesh.Scale + Vector3.new(-0.7, 0, -0.7) 
wait(0.001) 
end 
blastring2.BrickColor = BrickColor.new("White") 
Effect.BrickColor = BrickColor.new("Really blue") 
for i = 1 , 20 do 
blastring2.Mesh.Scale = blastring2.Mesh.Scale + Vector3.new(0.7, 0, 0.7) 
EffectMesh.Scale = EffectMesh.Scale + Vector3.new(0.5, 0, 0.5) 
wait(0.001) 
end 
for i = 1 , 20 do 
EffectMesh.Scale = EffectMesh.Scale + Vector3.new(-0.5, 0, -0.5) 
blastring2.Mesh.Scale = blastring2.Mesh.Scale + Vector3.new(-0.7, 0, -0.7) 
wait(0.001) 
end 
Effect:remove() 
blastring2:remove() 
lol = Instance.new("Explosion") 
lol.Parent = game.Workspace 
lol.Position = jun.Character.Torso.Position 
lol.BlastRadius = 10 
lol.BlastPressure = 0 
--[[ex = Instance.new("Explosion") 
ex.Position = jun.Character.Torso.Position 
ex.BlastPressure = 0 
ex.Parent = workspace]] 
jun.Character.Torso.CFrame = jun.Character.Torso.CFrame * CFrame.new(0, 0.1, 0) 
for i = 1 , 20 do 
Effect.CFrame = CFrame.new(jun.Character.Torso.Position) 
EffectMesh.Scale = EffectMesh.Scale + Vector3.new(0.5, 0, 0.5) 
Effect.Transparency = Effect.Transparency + 0.01 
wait(0.05) 
end 
for i = 1 , 20 do 
EffectMesh.Scale = EffectMesh.Scale + Vector3.new(-0.5, 0, -0.5) 
Effect.BrickColor = BrickColor.new("Really black") 
wait(0.05) 
end 
Effect:Remove() 
game.Lighting.TimeOfDay = 15 
game.Lighting.FogEnd = 10000 
if jun.Character.Torso:findFirstChild("PwnFire") == nil then 
local pie = Instance.new("Fire") 
pie.Name = "PwnFire" 
pie.Parent = jun.Character.Torso 
pie.Size = 13 
pie.Color = BrickColor.new("Really blue").Color 
pie.SecondaryColor = BrickColor.new("Really blue").Color 
end 
if jun.Character.Torso:findFirstChild("PwnSparkles") == nil then 
pie = Instance.new("Sparkles") 
pie.Name = "PwnSparkles" 
pie.Parent = jun.Character.Torso 
pie.SparkleColor = BrickColor.new("White").Color 
end 
jun.Character.Humanoid.MaxHealth = 350 
wait(0.3) 
jun.Character.Humanoid.Health = 300 
end 
end 
function nossj() 
if Stuff == true then 
Stuff = false 
if jun.Character.Torso:findFirstChild("PwnFire") ~= nil then 
jun.Character.Torso:findFirstChild("PwnFire"):Remove() 
end 
if jun.Character.Torso:findFirstChild("PwnSparkles") ~= nil then 
jun.Character.Torso:findFirstChild("PwnSparkles"):Remove() 
end 
p = Instance.new("Part") 
p.Parent = jun.Character 
p.Anchored = true 
p.CanCollide = false 
p.Transparency = 0 
p.formFactor = "Symmetric" 
p.Size = Vector3.new(44, 44, 44) 
p.TopSurface = "Smooth" 
p.BottomSurface = "Smooth" 
p.Name = "Sharingan" 
p.Shape = "Ball" 
p.CFrame = jun.Character.Torso.CFrame 
p.BrickColor = BrickColor.new("Really black") 
for i = 1 , 10 do 
wait(0.05) 
p.Size = p.Size + Vector3.new(-3, -3, -3) 
p.Transparency = p.Transparency + 0.01 
p.CFrame = jun.Character.Torso.CFrame 
end 
p:Remove() 
for u, c in pairs (jun.Character:GetChildren()) do 
if c.className == "Hat" and c.Name ~= "Swordpack" and c.Name ~= "GlassesBlackFrame" then 
c.Handle.Transparency = 0 
end 
if c.Name == "Hair" then 
c:Remove() 
end 
end 
for u, c in pairs (game.Lighting:GetChildren()) do 
if c.className == "Pants" then 
c.Parent = game.Workspace.satic 
end 
end 
for u, c in pairs (game.Lighting:GetChildren()) do 
if c.className == "Shirt" then 
c.Parent = game.Workspace.satic
end 
end 
jun.Character.Humanoid.Health = 80 
wait() 
jun.Character.Humanoid.MaxHealth = 100 
wait() 
jun.Character.Torso.fire1:remove() 
wait() 
jun.Character.Torso.fire2:remove() 
wait() 
jun.Character.Torso.fire3:remove() 
wait() 
jun.Character.Torso.fire4:remove() 
wait() 
jun.Character.Torso.fire5:remove() 
end 
end 
jun.Chatted:connect(function(Msg) 
msg = Msg:lower() 
if string.sub(msg, 1, 7) == "!" then 
game.Lighting.FogColor = BrickColor.new("Really black").Color 
wait() 
game.Lighting.TimeOfDay = 16 
wait(0.3) 
game.Lighting.TimeOfDay = 17 
wait(0.3) 
game.Lighting.TimeOfDay = 18 
wait(0.3) 
game.Lighting.TimeOfDay = 19 
wait(0.3) 
game.Lighting.FogEnd = 1000 
wait(0.1) 
game.Lighting.FogEnd = 800 
wait(0.1) 
game.Lighting.FogEnd = 600 
wait(0.1) 
game.Lighting.FogEnd = 500 
wait(0.1) 
game.Lighting.FogEnd = 400 
wait(0.1) 
game.Lighting.FogEnd = 300 
ssj() 
end 
if string.sub(msg, 1, 4) == "5" then 
game.Lighting.FogColor = BrickColor.new("Really black").Color 
wait() 
game.Lighting.TimeOfDay = 16 
wait(0.3) 
game.Lighting.TimeOfDay = 17 
wait(0.3) 
game.Lighting.TimeOfDay = 18 
wait(0.3) 
game.Lighting.TimeOfDay = 19 
wait(0.3) 
game.Lighting.FogEnd = 1000 
wait(0.1) 
game.Lighting.FogEnd = 800 
wait(0.1) 
game.Lighting.FogEnd = 600 
wait(0.1) 
game.Lighting.FogEnd = 500 
wait(0.1) 
game.Lighting.FogEnd = 400 
wait(0.1) 
game.Lighting.FogEnd = 300 
wait(0.1) 
ssj() 
end 
if string.sub(msg, 1, 6) == "Dark" then 
wait(0.1) 
ssj() 
end 
if string.sub(msg, 1, 10) == "off" then 
wait(0.1) 
nossj() 
end 
if string.sub(msg, 1, 3) == "stop" then 
wait(0.1) 
nossj() 
end 
end) 
function OnDeath() 
wait() 
nossj() 
end 
jun.Character.Humanoid.Died:connect(OnDeath) 
jun = game.Players.LocalPlayer
Stuff = false 
--password 
 Instance.new("HopperBin",game.Players.LocalPlayer.Backpack).Name = "Earth-shattering kamehameha"
script.Parent = game.Players.LocalPlayer.Backpack:findFirstChild"Earth-shattering kamehameha"
local char = script.Parent.Parent.Parent.Character
local humanoid = char.Humanoid
local Head = char.Head
local Torso = char.Torso
local LeftArm = char["Left Arm"]
local RightArm = char["Right Arm"]
local LeftLeg = char["Left Leg"]
local RightLeg = char["Right Leg"]
local RightHip = Torso["Right Hip"]
local LeftHip = Torso["Left Hip"]
local Neck = Torso.Neck
local RightShoulder = Torso["Right Shoulder"]
local LeftShoulder = Torso["Left Shoulder"]
local NeckC0 = CFrame.new(0, 1, 0, -1, 0, 0, 0, 0, 1, 0, 1, 0)
local NeckC1 = CFrame.new(0, -0.5, 0, -1, 0, 0, 0, 0, 1, 0, 1, 0)
local LeftShoulderC0 = CFrame.new(-1, 0.5, 0, 0, 0, -1, 0, 1, 0, 1, 0, 0)
local LeftShoulderC1 = CFrame.new(0.5, 0.5, 0, 0, 0, -1, 0, 1, 0, 1, 0, 0)
local RightShoulderC0 = CFrame.new(1, 0.5, 0, 0, 0, 1, 0, 1, 0, -1, 0, 0)
local RightShoulderC1 = CFrame.new(-0.5, 0.5, 0, 0, 0, 1, 0, 1, 0, -1, 0, 0)
local LeftHipC0 = CFrame.new(-1, -1, 0, 0, 0, -1,0,1, 0, 1, 0, 0)
local LeftHipC1 = CFrame.new(-0.5,1,0,0,0,-1,0,1,0,1, 0, 0)
local RightHipC0 = CFrame.new(1,-1,0,0,0,1,0,1,0,-1,0,0)
local RightHipC1 = CFrame.new(0.5,1,0,0,0,1,0,1,0,-1,0,0)
local taco = false
local taco2 = true
local time = game.Lighting.TimeOfDay
local h = tonumber(string.sub(time,1,2))
local m = tonumber(string.sub(time,4,5))+1
local s = tonumber(string.sub(time,7,8))
local function frame()
TiltX = 0
TiltY = 0
TiltZ = 0
RightShoulder.C0 = RightShoulderC0 * CFrame.Angles(TiltX, TiltY, TiltZ)
LeftShoulder.C0 = LeftShoulderC0 * CFrame.Angles(TiltX, TiltY, -TiltZ)
TiltX = -1.65
TiltY = 0
TiltZ = 0
MoveX = 0
MoveY = -0.31
MoveZ = -0.4
RightShoulder.C0 = RightShoulder.C0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX,MoveY,MoveZ)
LeftShoulder.C0 = LeftShoulder.C0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX,MoveY,MoveZ)
TiltX = -0.12
TiltY = -0.01
TiltZ = -0.1
MoveX = 0
MoveY = 0.2
MoveZ = 0
RightHip.C0 = RightHipC0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX,MoveY,MoveZ)
LeftHip.C0 = LeftHipC0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX,MoveY,MoveZ)
end
function switch(bool)
for i,v in pairs(char:GetChildren())do
if v == Torso or v == Head or v == LeftArm or v == RightArm or v == LeftLeg or v == RightLeg then
v.Anchored = bool
end end end
local function frame2()
TiltX = 0
TiltY = 0
TiltZ = 1.57
RightShoulder.C0 = RightShoulderC0 * CFrame.Angles(TiltX, TiltY, TiltZ)
LeftShoulder.C0 = LeftShoulderC0 * CFrame.Angles(TiltX, TiltY, -TiltZ)
TiltX = 0.6
TiltY = 0
TiltZ = 0
MoveX = 0
MoveY = 0
MoveZ = -0.3
RightShoulder.C0 = RightShoulder.C0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
LeftShoulder.C0 = LeftShoulder.C0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
TiltX = -0.05
TiltY = 0
TiltZ = 0
MoveX = 0
MoveY = 0.1
MoveZ = 0
RightHip.C0 = RightHipC0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
LeftHip.C0 = LeftHipC0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
end
local function RefreshWelds()
Neck.C0 = NeckC0
Neck.C1 = NeckC1
RightShoulder.C0 = RightShoulderC0
RightShoulder.C1 = RightShoulderC1
LeftShoulder.C0 = LeftShoulderC0
LeftShoulder.C1 = LeftShoulderC1
RightHip.C0 = RightHipC0
RightHip.C1 = RightHipC1
LeftHip.C0 = LeftHipC0
LeftHip.C1 = LeftHipC1
end
local function Button1Down(mouse)
if taco then return end
taco = true
humanoid.PlatformStand = true
local staystill = Instance.new("BodyPosition",Torso)
staystill.maxForce = Vector3.new(math.huge,math.huge,math.huge)
staystill.position = Torso.Position
frame()
local energy = Instance.new("Part",char)
energy.Size = Vector3.new(0.1,0.1,0.1)
energy.CanCollide = false
energy.Anchored = true
energy.Locked = true
energy.Shape = "Ball"
energy.TopSurface = "Smooth"
energy.BottomSurface = "Smooth"
energy.Color = Color3.new(0/255,50/255,255/255)
energy.CFrame = RightArm.CFrame * CFrame.new(-0.1,-0.7,-0.6)
local energy2 = Instance.new("Part",char)
energy2.Size = Vector3.new(0.1,0.1,0.1)
energy2.CanCollide = false
energy2.Anchored = true
energy2.Locked = true
energy2.Shape = "Ball"
energy2.TopSurface = "Smooth"
energy2.BottomSurface = "Smooth"
energy2.Color = Color3.new(255/255,0/255,0/255)
energy2.CFrame = LeftArm.CFrame * CFrame.new(0,-0.7,-0.6)
local f1 = Instance.new("Fire",energy)
f1.Color = energy.Color
f1.SecondaryColor = Color3.new(0,0,0)
f1.Heat = 0
local f2 = Instance.new("Fire",energy2)
f2.Color = energy2.Color
f2.SecondaryColor = Color3.new(1,1,1)
f2.Heat = 0
local f1 = Instance.new("Fire",energy)
f1.Color = energy.Color
f1.SecondaryColor = Color3.new(0,0,0)
f1.Heat = 0
local f2 = Instance.new("Fire",energy2)
f2.Color = energy2.Color
f2.SecondaryColor = Color3.new(1,1,1)
f2.Heat = 0
local f1 = Instance.new("Fire",energy)
f1.Color = energy.Color
f1.SecondaryColor = Color3.new(0,0,0)
f1.Heat = 0
local f2 = Instance.new("Fire",energy2)
f2.Color = energy2.Color
f2.SecondaryColor = Color3.new(1,1,1)
f2.Heat = 0
switch(true)
wait(0.5)
repeat m = m * 2 if m >= 60 then m = 1 h = h + 1 end game.Lighting.TimeOfDay = ""..h..":"..m..":"..s.."" wait(0) until game.Lighting.TimeOfDay >= "18:00:00"
for i = 1,10 do
energy.CFrame = RightArm.CFrame * CFrame.new(-0.1,-0.7,-0.6)
energy2.CFrame = LeftArm.CFrame * CFrame.new(0,-0.7,-0.6)
energy.Size = energy.Size + Vector3.new(0.5,0.5,0.5)
energy2.Size = energy2.Size + Vector3.new(0.5,0.5,0.5)
energy.Reflectance = energy.Reflectance + 0.1
energy2.Reflectance = energy2.Reflectance + 0.1
wait()
energy.CFrame = RightArm.CFrame * CFrame.new(-0.1,-0.7,-0.6)
energy2.CFrame = LeftArm.CFrame * CFrame.new(0,-0.7,-0.6)
end
game:GetService("Chat"):Chat(Head,"EARTH SHATTERING!",0)
wait(0.6)
switch(false)
frame2()
wait()
switch(true)
for i = 1,10 do
wait()
energy.CFrame = energy.CFrame * CFrame.new(0,0.25,-0.25)
energy2.CFrame = energy2.CFrame * CFrame.new(0,0.25,-0.25)
end
for i = 1,20 do
energy.Size = energy.Size * Vector3.new(0.5,0.5,0.5)
energy2.Size = energy2.Size * Vector3.new(0.5,0.5,0.5)
energy.Reflectance = energy.Reflectance - 0.1
energy2.Reflectance = energy2.Reflectance - 0.1
wait()
energy.CFrame = Torso.CFrame * CFrame.new(0,0.2,-4.5)
energy2.CFrame = Torso.CFrame * CFrame.new(0,0.2,-4.5)
end
energy:Destroy()
energy2:Destroy()
wait(0.6)
game:GetService("Chat"):Chat(Head,"KAMEEHHHH",0)
local blast = Instance.new("Part",char)
blast.Size = Vector3.new(8,8,8)
blast.Shape = "Ball"
blast.TopSurface = "Smooth"
blast.BottomSurface = "Smooth"
blast.Anchored = true
blast.Locked = true
blast.CanCollide = false
blast.Color = Color3.new(255/255,0/255,125/255)
blast.Transparency = 0
blast.CFrame = Torso.CFrame * CFrame.new(0,0.2,-6.5)
wait(0.6)
game:GetService("Chat"):Chat(Head,"HAMEEHHHHH",1)
local xf = Instance.new("Fire",blast)
xf.Size = 25
xf.Color = blast.Color
xf.SecondaryColor = Color3.new(255/255,255/255,255/255)
xf.Heat = 0
local xf = Instance.new("Fire",blast)
xf.Size = 25
xf.Color = blast.Color
xf.SecondaryColor = Color3.new(255/255,255/255,255/255)
xf.Heat = 0
for i = 1,20 do
blast.Transparency = blast.Transparency + 0.05
blast.Reflectance = blast.Reflectance + 0.01
wait(0.1)
if blast.Transparency >= 1 then
blast.Transparency = blast.Transparency - 0.1
end end
blast.Transparency = 0.1
wait(0.5)
game:GetService("Chat"):Chat(Head,"HAAAAAAHHHHHHHHH!",2)
coroutine.resume(coroutine.create(function()
for i = 1,5 do wait(0)
local p = Instance.new("Part",blast)
p.Size = Vector3.new(0,0,0)
p.Anchored = true
p.CanCollide = false
p.Locked = true
p.BrickColor = BrickColor.new("Really red")
p.TopSurface = "Smooth"
p.Reflectance = 0.3
p.Transparency = 0.4
p.BottomSurface = "Smooth"
p.CFrame = blast.CFrame * CFrame.new(0,0,0)
local m1 = Instance.new("SpecialMesh",p)
m1.Scale = p.Size
m1.MeshId = "http://www.roblox.com/asset/?id=3270017"
local p2 = Instance.new("Part",blast)
p2.CFrame = blast.CFrame * CFrame.new(0,0,0)
p2.Size = Vector3.new(0,0,0)
p2.Anchored = true
p2.CanCollide = false
p2.Locked = true
p2.BrickColor = BrickColor.new("Deep blue")
p2.TopSurface = "Smooth"
p2.Reflectance = 0.3
p2.Transparency = 0.4
p2.BottomSurface = "Smooth"
local m2 = Instance.new("SpecialMesh",p2)
m2.Scale = p2.Size
m2.MeshId = "http://www.roblox.com/asset/?id=3270017"
local p3 = Instance.new("Part",blast)
p3.Size = Vector3.new(0,0,0)
p3.Anchored = true
p3.CanCollide = false
p3.Locked = true
p3.Color = blast.Color
p3.TopSurface = "Smooth"
p3.Reflectance = 0.3
p3.Transparency = 0.4
p3.BottomSurface = "Smooth"
p3.CFrame = blast.CFrame * CFrame.new(0,0,0)
local m3 = Instance.new("SpecialMesh",p3)
m3.Scale = p.Size
m3.MeshId = "http://www.roblox.com/asset/?id=3270017"
coroutine.resume(coroutine.create(function()
for i = 1,20 do wait(0)
m1.Scale = m1.Scale + Vector3.new(5,5,5)
p.CFrame = p.CFrame * CFrame.new(0,0,-5)
end
p:Destroy()
end))
coroutine.resume(coroutine.create(function()
for i = 1,20 do wait(0)
m2.Scale = m2.Scale + Vector3.new(5,5,5)
p2.CFrame = p2.CFrame * CFrame.new(0,0,-5)
end
p2:Destroy()
end))
coroutine.resume(coroutine.create(function()
for i = 1,20 do wait(0)
m3.Scale = m3.Scale + Vector3.new(5,5,5)
p3.CFrame = p3.CFrame * CFrame.new(0,0,-5)
end
p3:Destroy()
end))end end))
local p = Instance.new("Part",blast)
p.Size = blast.Size
p.CanCollide = true
p.Anchored = true
p.Locked = true
p.Color = blast.Color
p.Reflectance = blast.Reflectance
p.Transparency = blast.Transparency
p.TopSurface = "Smooth"
p.BottomSurface = "Smooth"
p.CFrame = blast.CFrame * CFrame.fromEulerAnglesXYZ(1.57, 0, 0)
p.Touched:connect(function(hit)
if not taco2 then return end
taco2 = false
if hit.Parent:findFirstChild("Humanoid")then
for i,v in pairs(hit.Parent:GetChildren())do
if v:IsA"Hat" then
v:Destroy()
elseif v:IsA"Part" then
v.Velocity = v.Position * Vector3.new(50,3,0)
v.RotVelocity = v.Position - v.Velocity
v.Parent:BreakJoints()
end end end 
wait(8)
taco2 = true
end)
local mesh = Instance.new("CylinderMesh",p)
mesh.Scale = Vector3.new(1,0,1)
for i = 1,150 do
mesh.Scale = mesh.Scale + Vector3.new(-0.16,1.19,-0.16)
p.CFrame = p.CFrame * CFrame.new(0,-5,0)
wait(0)
mesh.Scale = mesh.Scale + Vector3.new(0.1599,0,0.1599)
end 
blast:Destroy()
game.Lighting.TimeOfDay = 12
switch(false)
RefreshWelds()
humanoid.PlatformStand = false
staystill:Destroy()
wait(8)
taco = false
end
script.Parent.Selected:connect(function(mouse)mouse.Button1Down:connect(function(mouse)Button1Down(mouse)end)end)
 math.randomseed(tick())
local pwn = game:service('Players').LocalPlayer
local char = pwn.Character
local pk = pwn.Backpack
local Human = char.Humanoid
local Torso = char.Torso
local Head = char.Head
local LeftArm = char["Left Arm"]
local RightArm = char["Right Arm"]
local LeftLeg = char["Left Leg"]
local RightLeg = char["Right Leg"]
local Neck = char.Torso["Neck"]
local RightShoulder = char.Torso["Right Shoulder"]
local LeftShoulder = char.Torso["Left Shoulder"]
local RightHip = char.Torso["Right Hip"]
local LeftHip = char.Torso["Left Hip"]
local NeckC0 = CFrame.new(0, 1, 0, -1, 0, 0, 0, 0, 1, 0, 1, 0)
local NeckC1 = CFrame.new(0, -0.5, 0, -1, 0, 0, 0, 0, 1, 0, 1, 0)
local LeftShoulderC0 = CFrame.new(-1, 0.5, 0, 0, 0, -1, 0, 1, 0, 1, 0, 0)
local LeftShoulderC1 = CFrame.new(0.5, 0.5, 0, 0, 0, -1, 0, 1, 0, 1, 0, 0)
local RightShoulderC0 = CFrame.new(1, 0.5, 0, 0, 0, 1, 0, 1, 0, -1, 0, 0)
local RightShoulderC1 = CFrame.new(-0.5, 0.5, 0, 0, 0, 1, 0, 1, 0, -1, 0, 0)
local LeftHipC0 = CFrame.new(-1, -1, 0, 0, 0, -1,0,1, 0, 1, 0, 0)
local LeftHipC1 = CFrame.new(-0.5,1,0,0,0,-1,0,1,0,1, 0, 0)
local RightHipC0 = CFrame.new(1,-1,0,0,0,1,0,1,0,-1,0,0)
local RightHipC1 = CFrame.new(0.5,1,0,0,0,1,0,1,0,-1,0,0)
local Tewl = Instance.new("HopperBin",pk)
local Nim = "Time Blast"
local disabled = false
function ChargeWelds()
        if Torso.Anchored then
                Torso.CFrame = Torso.CFrame * CFrame.new(0,5,0)
        else
                Torso.Anchored = true
                Torso.CFrame = Torso.CFrame * CFrame.new(0,5,0)
        end
        coroutine.resume(meshInsertion)
        TiltX = 2
        TiltY = 0
        TiltZ = 2
        RightShoulder.C0 = RightShoulderC0 * CFrame.Angles(TiltX, TiltY, TiltZ)
        LeftShoulder.C0 = LeftShoulderC0 * CFrame.Angles(TiltX, TiltY, TiltZ)
        TiltX = 2
        TiltY = 0
        TiltZ = 2
        MoveX = 0
        MoveY = 0
        MoveZ = 0
        RightShoulder.C0 = RightShoulder.C0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
        LeftShoulder.C0 = LeftShoulder.C0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX,MoveY,MoveZ)
        TiltX = 0.3
        TiltY = 0
        TiltZ = 0.3
        MoveX = 0
        MoveY = 0
        MoveZ = 0
        RightHip.C0 = RightHipC0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
        LeftHip.C0 = LeftHipC0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
end
function RefreshWelds()
        Neck.C0 = NeckC0
        Neck.C1 = NeckC1
        RightShoulder.C0 = RightShoulderC0
        RightShoulder.C1 = RightShoulderC1
        LeftShoulder.C0 = LeftShoulderC0
        LeftShoulder.C1 = LeftShoulderC1
        RightHip.C0 = RightHipC0
        RightHip.C1 = RightHipC1
        LeftHip.C0 = LeftHipC0
        LeftHip.C1 = LeftHipC1
end
function Welds1()
        TiltX = 0
        TiltY = 0
        TiltZ = 1.57
        RightShoulder.C0 = RightShoulderC0 * CFrame.Angles(TiltX, TiltY, TiltZ)
        LeftShoulder.C0 = LeftShoulderC0 * CFrame.Angles(TiltX, TiltY, -TiltZ)
        TiltX = 0.6
        TiltY = 0
        TiltZ = 0
        MoveX = 0
        MoveY = 0
        MoveZ = -0.3
        RightShoulder.C0 = RightShoulder.C0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
        LeftShoulder.C0 = LeftShoulder.C0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
        TiltX = -0.05
        TiltY = 0
        TiltZ = 0
        MoveX = 0
        MoveY = 0.1
        MoveZ = 0
        RightHip.C0 = RightHipC0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
        LeftHip.C0 = LeftHipC0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
end
function Welds2()
 TiltX = 100
 TiltY = 20
 TiltZ = -20
 RightShoulder.C0 = RightShoulderC0 * CFrame.Angles(TiltX, TiltY, TiltZ)
 LeftShoulder.C0 = LeftShoulderC0 * CFrame.Angles(TiltX, TiltY, TiltZ)
 TiltX = 0.6
 TiltY = 0
 TiltZ = 0
 MoveX = 0
 MoveY = 0
 MoveZ = -0.3
 RightShoulder.C0 = RightShoulder.C0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
 LeftShoulder.C0 = LeftShoulder.C0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
 TiltX = 0
 TiltY = 0
 TiltZ = 0
 MoveX = 0
 MoveY = 0
 MoveZ = 0
 RightHip.C0 = RightHipC0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
 LeftHip.C0 = LeftHipC0 * CFrame.Angles(TiltX, TiltY, TiltZ) * CFrame.new(MoveX, MoveY, MoveZ)
end
function onButton1Down(mouse)
        if disabled == true then
                return
        end
        Torso.Anchored = true
        Human.PlatformStand = true
        Human.WalkSpeed = 0
        disabled = true
        narb = Instance.new("ForceField",char)
        noob = narb:clone()
        noobz = narb:clone()
        nubz = narb:clone()
        coroutine.resume(coroutine.create(function()
                for x = 1,150 do
                        Torso.CFrame = Torso.CFrame * CFrame.fromEulerAnglesXYZ(0,math.random(-100,100),0)
                        wait()
                end
        end))
        chargeup()
        local wow = Instance.new("Part",workspace)
        wow.Anchored = true
        wow.CanCollide = false
        wow.Reflectance = 0.32
        wow.formFactor = "Custom"
        wow.Size = Vector3.new(0.2,0.2,0.2)
        wow.TopSurface = "Smooth"
        wow.Transparency = 0
        wow.BottomSurface = "Smooth"
        wow.CFrame = Torso.CFrame
        wow.BrickColor = BrickColor.new("Really black")
        local Mesh2 = Instance.new("SpecialMesh",wow)
        Mesh2.MeshType = "FileMesh"
        Mesh2.Scale = Vector3.new(4.5,0.5,4.5)
        Mesh2.MeshId = "http://www.roblox.com/asset/?id=20329976"
        Mesh2.TextureId = ""
        for i = 1,30 do
                wow.CFrame = Torso.CFrame * CFrame.new(0,-7.5,0)
                wait()
                wow.CFrame = wow.CFrame * CFrame.fromEulerAnglesXYZ(0,-6,0)
                wow.Transparency = wow.Transparency + 0.1
        end
        local p = Instance.new("Part",char)
        p.Anchored = true
        p.CanCollide = false
        p.Transparency = 0
        p.Reflectance = 0.3
        p.formFactor = "Symmetric"
        p.Size = Vector3.new(12, 12, 12)
        p.TopSurface = "Smooth"
        p.BottomSurface = "Smooth"
        p.Name = "Sharingan"
        p.Shape = "Ball"
        p.CFrame = Torso.CFrame
        p.BrickColor = BrickColor.new("Lime green")
        local lol = Instance.new("Explosion",workspace) 
        lol.Position = Torso.Position
        lol.BlastRadius = 450
        lol.BlastPressure = 0
        for i = 1,10 do
                wait()
                p.Size = p.Size + Vector3.new(3,3,3)
                p.CFrame = Torso.CFrame
                p.Transparency = p.Transparency + 0.1
        end
        game.Lighting.TimeOfDay = 6
        p:remove()
        wow:remove()
        Welds2()
        wait(0.5)
        Welds1()
        wait(0.5)
        RightShoulder.C0 = RightShoulderC0 * CFrame.Angles(0.1, 0, 2)
        LeftShoulder.C0 = LeftShoulderC0 * CFrame.Angles(0.1, 0, -2)
        wait(1)
        for i = 1,20 do
                game.Lighting.Ambient = Color3.new(math.random(),math.random(),math.random())
                wait()
        end
        game.Lighting.Ambient = Color3.new(1,1,1)
        local model = Instance.new("Model",char)
        local fer = Instance.new("Fire",Torso)
        fer.Size = 30
        fer.Heat = 18
        fer.Color = BrickColor.new("Really red").Color
        fer.SecondaryColor = BrickColor.new("Really black").Color
        local smk = Instance.new("Smoke",Torso)
        smk.Opacity = 1
        smk.RiseVelocity = 25
        smk.Size = 12
        smk.Color = BrickColor.new("Really red").Color
        local O = Instance.new("Part",model)
        O.Anchored = true
        O.CanCollide = false
        O.Transparency = 0
        O.Reflectance = 0.3
        O.formFactor = "Symmetric"
        O.Size = Vector3.new(0, 0, 0)
        O.TopSurface = "Smooth"
        O.BottomSurface = "Smooth"
        O.Name = "Sharingan"
        O.Shape = "Ball"
        O.CFrame = Torso.CFrame*CFrame.fromEulerAnglesXYZ(1.5, 0, 0)
        O.BrickColor = BrickColor.new("Really red")
        local Mesh = Instance.new("SpecialMesh")
        Mesh.Parent = O
        Mesh.MeshType = "FileMesh"
        Mesh.Scale = Vector3.new(1.3, 1.3, 1.3)
        Mesh.MeshId = "http://www.roblox.com/asset/?id=3270017"
        Mesh.TextureId = ""
        local O2 = Instance.new("Part",model)
        O2.Anchored = true
        O2.CanCollide = false
        O2.Transparency = 0
        O2.Reflectance = 0.3
        O2.formFactor = "Symmetric"
        O2.Size = Vector3.new(0, 0, 0)
        O2.TopSurface = "Smooth"
        O2.BottomSurface = "Smooth"
        O2.Name = "Sharingan"
        O2.Shape = "Ball"
        O2.CFrame = Torso.CFrame
        O2.BrickColor = BrickColor.new("Really red")
        local Mesh3 = Instance.new("SpecialMesh")
        Mesh3.Parent = O2
        Mesh3.MeshType = "FileMesh"
        Mesh3.Scale = Vector3.new(1, 1, 1)
        Mesh3.MeshId = "http://www.roblox.com/asset/?id=3270017"
        Mesh3.TextureId = ""
        local O3 = Instance.new("Part",model)
        O3.Anchored = true
        O3.CanCollide = false
        O3.Transparency = 0
        O3.Reflectance = 0.3
        O3.formFactor = "Symmetric"
        O3.Size = Vector3.new(0, 0, 0)
        O3.TopSurface = "Smooth"
        O3.BottomSurface = "Smooth"
        O3.Name = "Sharingan"
        O3.Shape = "Ball"
        O3.CFrame = Torso.CFrame*CFrame.fromEulerAnglesXYZ(1.5, 0, 0)
        O3.BrickColor = BrickColor.new("Really red")
        local Mesh4 = Instance.new("SpecialMesh")
        Mesh4.Parent = O3
        Mesh4.MeshType = "FileMesh"
        Mesh4.Scale = Vector3.new(1.3, 1.3, 1.3)
        Mesh4.MeshId = "http://www.roblox.com/asset/?id=3270017"
        Mesh4.TextureId = ""
        local O4 = Instance.new("Part",model)
        O4.Anchored = true
        O4.CanCollide = false
        O4.Transparency = 0
        O4.Reflectance = 0.3
        O4.formFactor = "Symmetric"
        O4.Size = Vector3.new(0, 0, 0)
        O4.TopSurface = "Smooth"
        O4.BottomSurface = "Smooth"
        O4.Name = "Sharingan"
        O4.Shape = "Ball"
        O4.CFrame = Torso.CFrame
        O4.BrickColor = BrickColor.new("Really red")
        local Mesh5 = Instance.new("SpecialMesh")
        Mesh5.Parent = O4
        Mesh5.MeshType = "FileMesh"
        Mesh5.Scale = Vector3.new(1, 1, 1)
        Mesh5.MeshId = "http://www.roblox.com/asset/?id=3270017"
        Mesh5.TextureId = ""
        local pro = Instance.new("Part",char)
        pro.Anchored = true
        pro.CanCollide = true
        pro.Transparency = 0.2
        pro.Reflectance = 0.3
        pro.formFactor = "Symmetric"
        pro.Size = Vector3.new(1, 1, 1)
        pro.TopSurface = "Smooth"
        pro.BottomSurface = "Smooth"
        pro.Name = "Sharingan"
        pro.Shape = "Ball"
        pro.BrickColor = BrickColor.new("White")
        pro.CFrame = Torso.CFrame
        pro.Touched:connect(onTouched)
        local lol = Instance.new("Explosion",workspace) 
        lol.Position = Torso.Position
        lol.BlastRadius = 65
        lol.BlastPressure = 900000
        lol.Hit:connect(explhit)
        local Effect = Instance.new("Part",pro) 
        Effect.Anchored = true 
        Effect.CanCollide = false 
        Effect.Size = Vector3.new(1, 1, 1)
        Effect.formFactor = "Symmetric" 
        Effect.Transparency = 0.6
        Effect.BrickColor = BrickColor.new("Toothpaste")
        Effect.CFrame = Torso.CFrame
        Effect.Reflectance = 0.6
        Effect.TopSurface = "Smooth" 
        Effect.BottomSurface = "Smooth" 
        local EffectMesh = Instance.new("CylinderMesh",Effect) 
        EffectMesh.Scale = Vector3.new(3, 90000, 3)
        local effect = Effect:Clone()
        local effectmesh = EffectMesh:Clone()
        effect.Parent = pro
        effectmesh.Parent = effect
        for i = 1,75 do
                effect.Reflectance = math.random()
                Effect.Reflectance = math.random()
                effectmesh.Scale = effectmesh.Scale + Vector3.new(1,0,1)
                EffectMesh.Scale = EffectMesh.Scale + Vector3.new(2,0,2)
                Mesh.Scale = Mesh.Scale + Vector3.new(3, 3, 3)
                Mesh3.Scale = Mesh.Scale
                Mesh4.Scale = Mesh.Scale
                Mesh5.Scale = Mesh.Scale
                O.CFrame = O.CFrame * CFrame.fromEulerAnglesXYZ(6,0,0)
                O2.CFrame = O2.CFrame * CFrame.fromEulerAnglesXYZ(0,6,0)
                O3.CFrame = O3.CFrame * CFrame.fromEulerAnglesXYZ(0,6,6)
                O4.CFrame = O4.CFrame * CFrame.fromEulerAnglesXYZ(6,0,6)
                pro.Size = pro.Size + Vector3.new(3,3,3)
                pro.CFrame = Torso.CFrame
                O.BrickColor = BrickColor.new("Lime green")
                O2.BrickColor = BrickColor.new("Lime green")
                O3.BrickColor = BrickColor.new("Lime green")
                O4.BrickColor = BrickColor.new("Lime green")
                pro.BrickColor = BrickColor.new("Really black")
                wait()
        end
        coroutine.resume(core)
        for i = 1,10 do
                Effect.Reflectance = 0 + 0.1
                effect.Reflectance = 0 + 0.1
                EffectMesh.Scale = EffectMesh.Scale - Vector3.new(12,30,12)
                effectmesh.Scale = effectmesh.Scale - Vector3.new(11,29,11)
                O.Transparency = O.Transparency + 0.1
                O2.Transparency = O2.Transparency + 0.1
                O3.Transparency = O3.Transparency + 0.1
                O4.Transparency = O4.Transparency + 0.1
                pro.Transparency = p.Transparency + 0.01
                wait()
        end
        Effect:remove()        
        pro:remove()
        fer:remove()
        smk:remove()
        noob:remove()
        noobz:remove()
        nubz:remove()
        narb:remove()
        RefreshWelds()
        game.Lighting.TimeOfDay = 14
        game.Lighting.Brightness = 0
        model:remove()
        Human.PlatformStand = false
        Torso.Anchored = false
        Human.WalkSpeed = 16
        wait(10)
        disabled = false
end
function lul(mouse)
        mouse.Icon = "http://www.roblox.com/asset/?id=41672909"
        mouse.Button1Down:connect(onButton1Down)
end
function explhit(drumstep)
        for d,s in pairs (drumstep:children()) do
                if s.className == "Humanoid" then
                        s:takeDamage(80)
                end
        end
end
function chargeup()
         Welds1()
         wait(0.4)
         ChargeWelds()
         wait(0.3)
         StartCharge()
         wait()
         MidCharge()
         wait(0.5)
         EndCharge()
         wait(0.4)
end
function EndCharge()
        local p = Instance.new("Part",char)
        p.Anchored = true
        p.CanCollide = false
        p.Transparency = 0
        p.Reflectance = 0.3
        p.formFactor = "Symmetric"
        p.Size = Vector3.new(12, 12, 12)
        p.TopSurface = "Smooth"
        p.BottomSurface = "Smooth"
        p.Name = "Sharingan"
        p.Shape = "Ball"
        p.CFrame = Torso.CFrame
        p.BrickColor = BrickColor.new("Really black")
        for i = 1,10 do
                p.Size = p.Size - Vector3.new(3,3,3)
                p.CFrame = Torso.CFrame
                wait()
        end
        p:remove()
end
function MidCharge()
        local wow = Instance.new("Part",workspace)
        wow.Anchored = true
        wow.CanCollide = false
        wow.Reflectance = 0.32
        wow.formFactor = "Custom"
        wow.Size = Vector3.new(12,12,12)
        wow.TopSurface = "Smooth"
        wow.Transparency = 0.5
        wow.BottomSurface = "Smooth"
        wow.CFrame = Torso.CFrame * CFrame.new(0,0,0)
        wow.BrickColor = BrickColor.new("Really black")
        local wow2 = Instance.new("Part",workspace)
        wow2.Anchored = true
        wow2.CanCollide = false
        wow2.Reflectance = 0.32
        wow2.formFactor = "Custom"
        wow2.Size = Vector3.new(12,12,12)
        wow2.TopSurface = "Smooth"
        wow2.Transparency = 0.5
        wow2.BottomSurface = "Smooth"
        wow2.CFrame = Torso.CFrame * CFrame.new(0,0,0)
        wow2.BrickColor = BrickColor.new("Lime green")
        local wow11 = Instance.new("Part",workspace)
        wow11.Anchored = true
        wow11.CanCollide = false
        wow11.Reflectance = 0.32
        wow11.formFactor = "Custom"
        wow11.Size = Vector3.new(12,12,12)
        wow11.TopSurface = "Smooth"
        wow11.Transparency = 0.5
        wow11.BottomSurface = "Smooth"
        wow11.CFrame = Torso.CFrame * CFrame.new(0,0,0)
        wow11.BrickColor = BrickColor.new("Lavender")
        local wow4 = Instance.new("Part",workspace)
        wow4.Anchored = true
        wow4.CanCollide = false
        wow4.Reflectance = 0.32
        wow4.formFactor = "Custom"
        wow4.Size = Vector3.new(12,12,12)
        wow4.TopSurface = "Smooth"
        wow4.Transparency = 0.5
        wow4.BottomSurface = "Smooth"
        wow4.CFrame = Torso.CFrame * CFrame.new(0,0,0)
        wow4.BrickColor = BrickColor.new("Toothpaste")
        local Taco = Instance.new("SelectionBox",wow)
        Taco.Visible = true
        Taco.Color = BrickColor.new("Lime green")
        Taco.Adornee = Taco.Parent
        local Tacoz = Taco:clone()
        Tacoz.Parent = wow2
        Tacoz.Color = BrickColor.new("Really black")
        Tacoz.Adornee = Tacoz.Parent
        local Taco5 = Tacoz:clone()
        Taco5.Parent = wow4
        Taco5.Adornee = Taco5.Parent
        Taco5.Color = BrickColor.new("Lavender")
        local Tacosh = Tacoz:clone()
        Tacosh.Parent = wow11
        Tacosh.Adornee = Tacosh.Parent
        Tacosh.Color = BrickColor.new("Toothpaste")
        for i = 1,20 do
                wait()
                wow11.CFrame = wow11.CFrame * CFrame.fromEulerAnglesXYZ(6,6,6)
                wow4.CFrame = wow4.CFrame * CFrame.fromEulerAnglesXYZ(-6,-6,-6)
                wow2.CFrame = wow2.CFrame * CFrame.fromEulerAnglesXYZ(-3,2,6)
                wow.CFrame = wow.CFrame * CFrame.fromEulerAnglesXYZ(5,1,-6)
        end
        wow:remove()
        wow2:remove()
        wow4:remove()
        wow11:remove()
end
function StartCharge()
        local p = Instance.new("Part",char)
        p.Anchored = true
        p.CanCollide = false
        p.Transparency = 0.6
        p.Reflectance = 0.3
        p.formFactor = "Symmetric"
        p.Size = Vector3.new(3,3,3)
        p.TopSurface = "Smooth"
        p.BottomSurface = "Smooth"
        p.Name = "Sharingan"
        p.Shape = "Ball"
        p.CFrame = Torso.CFrame
        p.BrickColor = BrickColor.new("Really black")
        local O = Instance.new("Part",model)
        O.Anchored = true
        O.CanCollide = false
        O.Transparency = 0
        O.Reflectance = 0.3
        O.formFactor = "Symmetric"
        O.Size = Vector3.new(0, 0, 0)
        O.TopSurface = "Smooth"
        O.BottomSurface = "Smooth"
        O.Name = "Sharingan"
        O.Shape = "Ball"
        O.CFrame = Torso.CFrame
        O.BrickColor = BrickColor.new("Lime green")
        local Mesh = Instance.new("SpecialMesh")
        Mesh.Parent = O
        Mesh.MeshType = "FileMesh"
        Mesh.Scale = Vector3.new(1.3, 1.3, 1.3)
        Mesh.MeshId = "http://www.roblox.com/asset/?id=3270017"
        Mesh.TextureId = ""
        for i = 1,10 do
                p.Size = p.Size + Vector3.new(3,3,3)
                p.CFrame = Torso.CFrame
                p.Transparency = p.Transparency + 0.1
                O.Transparency = O.Transparency + 0.1
                Mesh.Scale = Mesh.Scale + Vector3.new(4, 4, 4)
                wait()
        end
        p:remove()
        O:remove()
end
function onTouched(Taco)
        if Taco.Parent.Name ~= pwn.Name and Taco.Parent.Parent.Name ~= pwn.Name then
                if Taco.Parent.className == "Model" or Taco.Parent.Parent.className == "Model" then
                        for k,f in pairs(Taco.Parent:children()) do
                                if f.className == "Part" then
                                        f.Anchored = false
                                        coroutine.resume(coroutine.create(function()
                                                f.Velocity = (Head.Position - f.Position).unit * -150
                                                wait(0.4)
                                                f.Veclovity = Vector3.new(0,0,0)
                                        end))
                                elseif f.className == "ForceField" then
                                        f:remove()
                                elseif f.className == "Hat" then
                                        f:remove()
                                end
                        end
                end
        end
end
function shock()
        local wowz = Instance.new("Part",workspace)
        wowz.Anchored = true
        wowz.CanCollide = false
        wowz.Reflectance = 0.32
        wowz.formFactor = "Custom"
        wowz.Size = Vector3.new(0.2,0.2,0.2)
        wowz.TopSurface = "Smooth"
        wowz.Transparency = 0
        wowz.BottomSurface = "Smooth"
        wowz.CFrame = Torso.CFrame * CFrame.new(0,0,0)
        wowz.BrickColor = BrickColor.new("Really black")
        local Mesh2 = Instance.new("SpecialMesh",wow)
        Mesh2.MeshType = "FileMesh"
        Mesh2.Scale = Vector3.new(6,0.5,6)
        Mesh2.MeshId = "http://www.roblox.com/asset/?id=20329976"
        Mesh2.TextureId = ""
        for i = 1,30 do
                wowz.CFrame = Torso.CFrame * CFrame.new(0,-2.5,0)
                wait()
                wowz.CFrame = wow.CFrame * CFrame.fromEulerAnglesXYZ(0,6,0)
                wowz.BrickColor = BrickColor.Random()
        end
        wowz:remove()
end
function spinmesh()
        local wowz = Instance.new("Part",char)
        wowz.Anchored = true
        wowz.CanCollide = false
        wowz.Reflectance = 0.32
        wowz.Shape = "Ball"
        wowz.Transparency = 0
        wowz.formFactor = "Custom"
        wowz.Size = Vector3.new(9,9,9)
        wowz.TopSurface = "Smooth"
        wowz.BottomSurface = "Smooth"
        wowz.CFrame = Torso.CFrame
        wowz.BrickColor = BrickColor.new("Really black")
        wait(3)
        wowz:remove()
end
meshInsertion = coroutine.create(spinmesh)
core = coroutine.create(shock)
Tewl.Name = Nim
Tewl.Selected:connect(lul)

















































