"""Build exits.json: every loading zone placed in every map, with its trigger volume.

    python exits.py               writes ../data/exits.json (needs OOTMENU_NUP and zstandard)

A loading zone is a logic group (the layout's AiGroups) whose Logic is FieldDungeonEntranceSet. Its references
hold the exit (FieldAndDungeonEntranceEventRequestActor, whose NextBancParamName{Child,Adult} name the destination)
and the trigger, an Area actor. An Area is a unit shape standing on its origin, placed by its Translate, Rotate
(radians, X Y Z) and Scale like an air wall (airwalls.py): RigidBodyShapeType 1 (or none) a box from -0.5 to 0.5
across and 0 to 1 up; 3 a cylinder of radius 1 from 0 to 1 up; 0 (a sphere) is kept as a cylinder of radius 1 from
-1 to 1, as the collision viewer draws it. Shapes use airwalls.json's form, so gendata.py writes them the same way.

Used by levitate (levitate.c): through walls switches Link's collision off, and the trigger only sees him with it on,
so floating into one ends levitation there.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import byml  # noqa: E402
import gamefiles  # noqa: E402
from airwalls import LAYOUT_SUFFIX, _scale_of  # noqa: E402

OUT = os.path.join(os.path.dirname(HERE), "data", "exits.json")
ENTRANCE_LOGIC = "FieldDungeonEntranceSet"
EXIT_ACTOR = "FieldAndDungeonEntranceEventRequestActor"
SHAPE_SPHERE, SHAPE_BOX, SHAPE_CYLINDER = 0, 1, 3


def area_shape(area):
    """The Area's trigger in its own unscaled space, in airwalls.json's form."""
    shape_type = (area.get("Dynamic") or {}).get("RigidBodyShapeType", SHAPE_BOX)
    if shape_type == SHAPE_CYLINDER:
        return {"type": "cylinder", "a": [0.0, 1.0, 0.0], "b": [0.0, 0.0, 0.0], "radius": 1.0}
    if shape_type == SHAPE_SPHERE:
        return {"type": "cylinder", "a": [0.0, 1.0, 0.0], "b": [0.0, -1.0, 0.0], "radius": 1.0}
    return {"type": "box", "center": [0.0, 0.5, 0.0], "half": [0.5, 0.5, 0.5]}


def main():
    nup = gamefiles.nup()
    layouts = sorted(path for path in nup.index if path.startswith("romfs/Banc/") and path.endswith(LAYOUT_SUFFIX))
    maps = {}
    for path in layouts:
        stem = path.rsplit("/", 1)[1][:-len(LAYOUT_SUFFIX)]
        part = stem.rsplit("_", 1)[-1]
        map_name = stem.rsplit("_", 1)[0] if part in ("Static", "Dynamic") else stem
        try:
            root = byml.load(gamefiles.read(path))
        except Exception as e:  # a file this reader can't handle: report and go on
            print(f"skip {path}: {e}")
            continue
        if not isinstance(root, dict):
            continue
        actors = {placed.get("Hash"): placed for placed in root.get("Actors") or []}
        for group in root.get("AiGroups") or []:
            if ENTRANCE_LOGIC not in group.get("Logic", ""):
                continue
            refs = [actors.get(ref.get("Reference")) for ref in group.get("References") or []]
            exit_params = next(((placed.get("Dynamic") or {}) for placed in refs
                                if placed and placed.get("Gyaml") == EXIT_ACTOR), {})
            to = exit_params.get("NextBancParamNameChild") or exit_params.get("NextBancParamNameAdult") or "?"
            for area in refs:
                if not area or area.get("Gyaml") != "Area":
                    continue
                maps.setdefault(map_name, []).append({
                    "to": to, "part": part, "hash": area.get("Hash"),
                    "t": [float(v) for v in area.get("Translate", [0, 0, 0])],
                    "r": [float(v) for v in area.get("Rotate") or [0, 0, 0]],
                    "s": _scale_of(area),
                    "shapes": [area_shape(area)],
                })
    with open(OUT, "w") as f:
        json.dump({"source": "game files (tools/exits.py)", "maps": maps}, f, separators=(",", ":"))
    total = sum(len(exits) for exits in maps.values())
    print(f"{total} loading zones in {len(maps)} maps -> {OUT}")


if __name__ == "__main__":
    main()
