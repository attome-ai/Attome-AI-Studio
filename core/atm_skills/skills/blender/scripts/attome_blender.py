"""Helpers for scripts that Attome runs in Blender (blender.run). The engine puts this module beside the script: `ab` is already imported.

Everything here is plain `bpy`: a script may use bpy directly for anything these do not cover. Sizes are Blender units (1 unit = 1 m).
Angles are degrees. A scene is built in Python, saved as a .blend by blender.run (save: true), and drawn by blender.render.
"""
import math

import bpy
from mathutils import Vector


# ---- the scene -------------------------------------------------------------------------------------------------------------------------

def clear_scene():
    """Remove every object, mesh, material and light of the scene (the default cube, light and camera too)."""
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.lights, bpy.data.cameras, bpy.data.curves, bpy.data.images):
        for item in list(coll):
            if item.users == 0:
                coll.remove(item)


def set_render(width=1920, height=1080, fps=24, engine="eevee", samples=None, transparent=False, frames=None):
    """Size, rate and engine. engine: eevee (fast, good for most things), cycles (slow, physically exact), workbench (flat, instant).
    transparent: the background is not drawn (a PNG sequence keeps it, to lay over a video). frames: (start, end) or a count."""
    scene = bpy.context.scene
    r = scene.render
    r.resolution_x, r.resolution_y, r.resolution_percentage = int(width), int(height), 100
    r.fps = int(fps)
    wanted = {"eevee": "BLENDER_EEVEE", "cycles": "CYCLES", "workbench": "BLENDER_WORKBENCH"}.get(str(engine).lower(), str(engine))
    options = [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items]
    if wanted in options:
        r.engine = wanted
    elif wanted == "BLENDER_EEVEE":
        r.engine = next((o for o in options if o.startswith("BLENDER_EEVEE")), options[0])
    else:
        raise ValueError("engine must be eevee, cycles or workbench; this Blender has " + ", ".join(options))
    if samples is not None:
        if r.engine == "CYCLES":
            scene.cycles.samples = int(samples)
        elif hasattr(scene, "eevee"):
            scene.eevee.taa_render_samples = int(samples)
    r.film_transparent = bool(transparent)
    if frames is not None:
        start, end = (1, int(frames)) if isinstance(frames, int) else (int(frames[0]), int(frames[1]))
        scene.frame_start, scene.frame_end = start, end
    return scene


def use_gpu():
    """Let Cycles draw on the graphics card when it has one (best effort; a computer without one draws on the processor)."""
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        for kind in ("OPTIX", "CUDA", "HIP", "METAL", "ONEAPI"):
            try:
                prefs.compute_device_type = kind
                prefs.get_devices()
                if any(d.type != "CPU" for d in prefs.devices):
                    for d in prefs.devices:
                        d.use = d.type != "CPU"
                    bpy.context.scene.cycles.device = "GPU"
                    return kind
            except Exception:
                continue
    except Exception:
        pass
    return None


def world(color=(0.05, 0.05, 0.08), strength=1.0, hdri=None):
    """The light and the colour all around. A colour, or hdri: the path of an .hdr / .exr picture of a sky or a room."""
    w = bpy.context.scene.world or bpy.data.worlds.new("World")
    bpy.context.scene.world = w
    w.use_nodes = True
    nt = w.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputWorld")
    bg = nt.nodes.new("ShaderNodeBackground")
    if hdri:
        env = nt.nodes.new("ShaderNodeTexEnvironment")
        env.image = bpy.data.images.load(hdri)
        nt.links.new(env.outputs["Color"], bg.inputs["Color"])
    else:
        bg.inputs["Color"].default_value = (*color[:3], 1.0)
    bg.inputs["Strength"].default_value = strength
    nt.links.new(bg.outputs["Background"], out.inputs["Surface"])
    return w


def bloom(threshold=0.8, size=0.7, strength=0.8, kind="Bloom"):
    """A real glow round bright things (the compositor's glare): neon, light bulbs, hot metal. Without it emission is only a flat bright colour."""
    scene = bpy.context.scene
    ng = bpy.data.node_groups.new("AttomeGlow", "CompositorNodeTree")
    scene.compositing_node_group = ng
    ng.interface.new_socket("Image", in_out="OUTPUT", socket_type="NodeSocketColor")
    rl = ng.nodes.new("CompositorNodeRLayers")
    gl = ng.nodes.new("CompositorNodeGlare")
    out = ng.nodes.new("NodeGroupOutput")
    gl.inputs["Type"].default_value = kind
    gl.inputs["Threshold"].default_value = threshold
    gl.inputs["Size"].default_value = size
    gl.inputs["Strength"].default_value = strength
    ng.links.new(rl.outputs["Image"], gl.inputs["Image"])
    ng.links.new(gl.outputs["Image"], out.inputs["Image"])
    return gl


def dof(cam, focus, fstop=2.8):
    """Depth of field: what is at `focus` (an object or a point) is sharp, the rest softens. A lower fstop softens more."""
    cam.data.dof.use_dof = True
    cam.data.dof.aperture_fstop = fstop
    if hasattr(focus, "location"):
        cam.data.dof.focus_object = focus
    else:
        cam.data.dof.focus_distance = (Vector(focus) - Vector(cam.location)).length


def motion_blur(shutter=0.5):
    """Blur what moves fast, as a real camera would: it makes a quick move read as speed."""
    bpy.context.scene.render.use_motion_blur = True
    bpy.context.scene.render.motion_blur_shutter = shutter


def cut_camera(cam, shots):
    """Jump the camera between framings on given frames (a hard cut each time): shots = [(frame, location, target, lens), ...]."""
    for frame, loc, target, lens in shots:
        cam.location = loc
        look_at(cam, target)
        cam.data.lens = lens
        for path in ("location", "rotation_euler"):
            cam.keyframe_insert(path, frame=frame)
        cam.data.keyframe_insert("lens", frame=frame)
    for action_owner in (cam, cam.data):
        if action_owner.animation_data and action_owner.animation_data.action:
            for fc in _fcurves(action_owner.animation_data.action):
                for k in fc.keyframe_points:
                    k.interpolation = "CONSTANT"


# ---- things ----------------------------------------------------------------------------------------------------------------------------

def material(name="Material", color=(0.8, 0.8, 0.8), metallic=0.0, roughness=0.5, emission=None, emission_strength=5.0, alpha=1.0):
    """A surface: base colour (r, g, b 0..1), metallic, roughness; emission makes it glow; alpha below 1 makes it see-through."""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = (*color[:3], 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    if alpha < 1.0:
        bsdf.inputs["Alpha"].default_value = alpha
    if emission is not None:
        bsdf.inputs["Emission Color"].default_value = (*emission[:3], 1.0)
        bsdf.inputs["Emission Strength"].default_value = emission_strength
    return m


def _finish(obj, name, mat, location, rotation, scale):
    if name:
        obj.name = name
    obj.location = location
    obj.rotation_euler = [math.radians(a) for a in rotation]
    if scale is not None:
        obj.scale = scale if hasattr(scale, "__len__") else (scale, scale, scale)
    if mat is not None:
        obj.data.materials.clear()
        obj.data.materials.append(mat)
    return obj


def add(kind="cube", name=None, material_=None, location=(0, 0, 0), rotation=(0, 0, 0), scale=None, smooth=True, **options):
    """A shape: cube, sphere, cylinder, cone, torus, plane, monkey (a test head). options go to the bpy operator (size, radius, depth...)."""
    ops = {"cube": bpy.ops.mesh.primitive_cube_add, "sphere": bpy.ops.mesh.primitive_uv_sphere_add, "cylinder": bpy.ops.mesh.primitive_cylinder_add,
           "cone": bpy.ops.mesh.primitive_cone_add, "torus": bpy.ops.mesh.primitive_torus_add, "plane": bpy.ops.mesh.primitive_plane_add,
           "monkey": bpy.ops.mesh.primitive_monkey_add}
    ops[kind](**options)
    obj = bpy.context.active_object
    if smooth and kind not in ("cube", "plane"):
        bpy.ops.object.shade_smooth()
    return _finish(obj, name, material_, location, rotation, scale)


def text3d(text, size=1.0, depth=0.15, bevel=0.02, material_=None, location=(0, 0, 0), rotation=(90, 0, 0), align="CENTER", font=None, name="Text"):
    """Words as a solid shape: size (height of a capital, in units), depth (how thick), bevel (rounded edges). It stands upright, facing -Y, as
    a camera at y < 0 sees it. font: the path of a .ttf / .otf file, or None for Blender's own."""
    bpy.ops.object.text_add()
    obj = bpy.context.active_object
    obj.data.body = text
    obj.data.size = size
    obj.data.extrude = depth
    obj.data.bevel_depth = bevel
    obj.data.align_x = align
    obj.data.align_y = "CENTER"
    if font:
        obj.data.font = bpy.data.fonts.load(font)
    return _finish(obj, name, material_, location, rotation, None)


def import_model(path, location=(0, 0, 0), scale=1.0):
    """Bring in a model file: .glb .gltf .obj .fbx .stl .ply .usd .abc. Returns the new objects."""
    before = set(bpy.data.objects)
    ext = path.lower().rsplit(".", 1)[-1]
    if ext in ("glb", "gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == "obj":
        bpy.ops.wm.obj_import(filepath=path)
    elif ext == "fbx":
        bpy.ops.import_scene.fbx(filepath=path)
    elif ext == "stl":
        bpy.ops.wm.stl_import(filepath=path)
    elif ext == "ply":
        bpy.ops.wm.ply_import(filepath=path)
    elif ext in ("usd", "usda", "usdc", "usdz"):
        bpy.ops.wm.usd_import(filepath=path)
    elif ext == "abc":
        bpy.ops.wm.alembic_import(filepath=path)
    else:
        raise ValueError("unknown model format ." + ext)
    new = [o for o in bpy.data.objects if o not in before]
    for o in new:
        if o.parent is None:
            o.location = Vector(o.location) + Vector(location)
            o.scale = Vector(o.scale) * scale
    return new


def picture_plane(path, width=1.6, location=(0, 0, 0), rotation=(90, 0, 0), emission=1.0, name="Picture"):
    """A flat picture in the scene (a photo, a screenshot, a frame of the film). It lights itself, so it shows as it is."""
    bpy.ops.mesh.primitive_plane_add()
    obj = bpy.context.active_object
    img = bpy.data.images.load(path)
    w, h = img.size
    obj.scale = (width / 2.0, width / 2.0 * h / max(1, w), 1.0)
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    emit = nt.nodes.new("ShaderNodeEmission")
    emit.inputs["Strength"].default_value = emission
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    nt.links.new(tex.outputs["Color"], emit.inputs["Color"])
    nt.links.new(emit.outputs["Emission"], out.inputs["Surface"])
    obj.data.materials.append(m)
    obj.name = name
    obj.location = location
    obj.rotation_euler = [math.radians(a) for a in rotation]
    return obj


# ---- camera and light ------------------------------------------------------------------------------------------------------------------

def camera(location=(0, -8, 2), target=(0, 0, 0.8), lens=50, name="Camera"):
    """A camera that looks at target (an object or a point) and is the scene's camera. lens: focal length in mm (35 wide, 50 normal, 85 close)."""
    cam = bpy.data.cameras.new(name)
    cam.lens = lens
    obj = bpy.data.objects.new(name, cam)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = location
    bpy.context.scene.camera = obj
    look_at(obj, target)
    return obj


def look_at(obj, target):
    """Turn obj so that it faces target (an object or a point)."""
    point = Vector(target.location if hasattr(target, "location") else target)
    direction = point - Vector(obj.location)
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def light(kind="area", location=(3, -3, 4), energy=500.0, color=(1, 1, 1), size=2.0, target=None, name="Light"):
    """A light: sun (energy about 3), area (soft, 300..2000 W), point, spot. target: an object or point it faces."""
    data = bpy.data.lights.new(name, kind.upper())
    data.energy = energy
    data.color = color[:3]
    if kind == "area":
        data.size = size
    elif kind in ("point", "spot"):
        data.shadow_soft_size = size / 4.0
    obj = bpy.data.objects.new(name, data)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = location
    if target is not None:
        look_at(obj, target)
    return obj


def three_point_lights(target=(0, 0, 1), strength=1.0):
    """The usual studio lighting: a bright key from the front left, a soft fill from the right, a rim from behind."""
    return [light("area", (-3.5, -4.0, 4.0), 900 * strength, (1.0, 0.96, 0.9), 3.0, target, "Key"),
            light("area", (4.0, -3.0, 1.5), 300 * strength, (0.85, 0.92, 1.0), 4.0, target, "Fill"),
            light("area", (0.0, 4.0, 3.5), 700 * strength, (1.0, 1.0, 1.0), 2.0, target, "Rim")]


# ---- movement --------------------------------------------------------------------------------------------------------------------------

def keyframe(obj, path, frame, value, interpolation="BEZIER"):
    """Set a property at a frame. path: location, rotation_euler (radians), scale, or any property such as 'data.energy'. value: a number or a list."""
    setattr_path = obj
    parts = path.split(".")
    for p in parts[:-1]:
        setattr_path = getattr(setattr_path, p)
    setattr(setattr_path, parts[-1], value)
    obj.keyframe_insert(data_path=path, frame=frame)
    anim = obj.animation_data.action if obj.animation_data else None
    if anim is not None:
        for fc in _fcurves(anim):
            for k in fc.keyframe_points:
                if k.co[0] == frame:
                    k.interpolation = interpolation


def _fcurves(action):
    if hasattr(action, "fcurves"):
        return action.fcurves
    found = []  # Blender 5: curves live in layered slots
    for layer in getattr(action, "layers", []):
        for strip in layer.strips:
            for bag in strip.channelbags:
                found.extend(bag.fcurves)
    return found


def linear(obj):
    """Make every movement of obj constant in speed (no ease in and out): a spin that must not slow down at the loop."""
    if obj.animation_data and obj.animation_data.action:
        for fc in _fcurves(obj.animation_data.action):
            for k in fc.keyframe_points:
                k.interpolation = "LINEAR"


def spin(obj, frames=120, turns=1.0, axis="Z"):
    """Turn obj round an axis, evenly, over frames (it ends where it began when turns is a whole number: it loops)."""
    i = "XYZ".index(axis.upper())
    start = list(obj.rotation_euler)
    end = list(start)
    end[i] += math.radians(360.0 * turns)
    keyframe(obj, "rotation_euler", 1, start)
    keyframe(obj, "rotation_euler", frames + 1, end)
    linear(obj)
    bpy.context.scene.frame_start, bpy.context.scene.frame_end = 1, frames


def orbit(cam, target=(0, 0, 0.8), radius=8.0, height=2.0, frames=120, turns=1.0, start_angle=-90.0):
    """Move cam round target on a circle (a turntable shot), looking at it all the time. start_angle -90 is the front (the -Y side)."""
    centre = Vector(target.location if hasattr(target, "location") else target)
    for f in range(0, frames + 1, max(1, frames // 60)):
        a = math.radians(start_angle + 360.0 * turns * f / frames)
        cam.location = (centre.x + radius * math.cos(a), centre.y + radius * math.sin(a), height)
        look_at(cam, centre)
        cam.keyframe_insert("location", frame=f + 1)
        cam.keyframe_insert("rotation_euler", frame=f + 1)
    linear(cam)
    bpy.context.scene.frame_start, bpy.context.scene.frame_end = 1, frames


def dolly(cam, start, end, frames=120, target=None):
    """Slide the camera from one point to another over frames (a push in or a pull out); target: where it keeps looking, else it keeps its angle."""
    for f, p in ((1, start), (frames + 1, end)):
        cam.location = p
        if target is not None:
            look_at(cam, target)
        cam.keyframe_insert("location", frame=f)
        if target is not None:
            cam.keyframe_insert("rotation_euler", frame=f)
    bpy.context.scene.frame_start, bpy.context.scene.frame_end = 1, frames


# ---- reading it back -------------------------------------------------------------------------------------------------------------------

def describe_scene():
    """A plain description of the scene, for an agent to read: objects with place and size, lights, the camera, render settings, frame range."""
    scene = bpy.context.scene
    objs = []
    for o in scene.objects:
        d = {"name": o.name, "type": o.type, "location": [round(v, 3) for v in o.location], "rotation_deg": [round(math.degrees(v), 1) for v in o.rotation_euler],
             "scale": [round(v, 3) for v in o.scale], "dimensions": [round(v, 3) for v in o.dimensions],
             "materials": [m.name for m in getattr(o.data, "materials", []) if m] if o.type in ("MESH", "CURVE", "FONT") else [],
             "animated": bool(o.animation_data and o.animation_data.action), "parent": o.parent.name if o.parent else None}
        if o.type == "LIGHT":
            d.update(light=o.data.type, energy=o.data.energy)
        if o.type == "CAMERA":
            d.update(lens=o.data.lens)
        if o.type == "FONT":
            d["text"] = o.data.body
        objs.append(d)
    r = scene.render
    return {"file": bpy.data.filepath, "objects": objs, "camera": scene.camera.name if scene.camera else None,
            "render": {"engine": r.engine, "width": r.resolution_x, "height": r.resolution_y, "percent": r.resolution_percentage, "fps": r.fps,
                       "transparent": r.film_transparent},
            "frames": {"start": scene.frame_start, "end": scene.frame_end, "count": scene.frame_end - scene.frame_start + 1},
            "world": scene.world.name if scene.world else None, "blender": bpy.app.version_string}
