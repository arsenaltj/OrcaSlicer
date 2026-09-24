"""Inspect actual ImageMap G-code and prepare a synchronized path comparison.

Offline visualization only: never executes G-code. Supports linear, planar,
metric extrusion with explicit G90/G91 and M82/M83 modes. Unsupported selected
surface arcs fail closed. Width recovery follows the local GCodeProcessor's
preview formula; rectangular beads are a display approximation, not optics or
an assertion about printed overhang shape/transmission.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import re
import numpy as np


SURFACES = {'Outer wall', 'Overhang wall', 'Top surface', 'Bottom surface'}
PARAM = re.compile(r'([XYZEFIJR])([-+]?(?:\d*\.\d+|\d+\.?\d*))')


def parse_gcode(text):
    lines = text.splitlines()
    metadata = {}
    for line in lines:
        if line.startswith('; ') and ' = ' in line:
            key, value = line[2:].split(' = ', 1)
            metadata[key] = value
    diameters = [float(v) for v in metadata.get('filament_diameter', '1.75').split(',')]
    xyz = np.full(3, np.nan); absolute = None; relative_e = None
    extrusion = 0.; tool = 0; layer = 0; height = None; forced_width = 0.
    role = ''; on_object = False; rows = []; roles = Counter(); tool_commands = []
    layers = defaultdict(set); layer_z = {}; unsupported = Counter()
    for number, line in enumerate(lines, 1):
        if line == ';LAYER_CHANGE': layer += 1
        elif line.startswith(';HEIGHT:'): height = float(line.split(':', 1)[1])
        elif line.startswith(';WIDTH:'): forced_width = float(line.split(':', 1)[1])
        elif line.startswith(';TYPE:'): role = line.split(':', 1)[1]
        elif line.startswith('; printing object '): on_object = True
        elif line.startswith('; stop printing object '): on_object = False
        command = line.split(';', 1)[0].strip()
        if not command: continue
        code = command.split()[0]
        p = {k: float(v) for k, v in PARAM.findall(command)}
        if code == 'G20': raise ValueError('Inch mode unsupported')
        if code == 'G90': absolute = True
        elif code == 'G91': absolute = False
        elif code == 'M82': relative_e = False
        elif code == 'M83': relative_e = True
        elif re.fullmatch(r'T\d+', code):
            tool = int(code[1:]); tool_commands.append(tool)
        elif code == 'G92':
            for i, axis in enumerate('XYZ'):
                if axis in p: xyz[i] = p[axis]
            if 'E' in p: extrusion = p['E']
        elif code in ('G0', 'G1', 'G2', 'G3'):
            before = xyz.copy()
            if any(axis in p for axis in 'XYZ') and absolute is None:
                raise ValueError(f'Missing coordinate mode at line {number}')
            for i, axis in enumerate('XYZ'):
                if axis in p: xyz[i] = p[axis] if absolute else xyz[i] + p[axis]
            delta_e = 0.
            if 'E' in p:
                if relative_e is None: raise ValueError('Missing extrusion mode')
                delta_e = p['E'] if relative_e else p['E'] - extrusion
                extrusion = extrusion + p['E'] if relative_e else p['E']
            selected = on_object and role in SURFACES and delta_e > 1e-9
            if code in ('G2', 'G3'):
                if selected: raise ValueError(f'Surface arc unsupported at line {number}')
                unsupported[code] += 1
                continue
            distance = float(np.linalg.norm(xyz - before))
            if not selected or not np.isfinite(distance) or distance < 1e-7: continue
            if abs(xyz[2]-before[2]) > 1e-4: raise ValueError('Nonplanar surface extrusion unsupported')
            if height is None or height <= 0: raise ValueError('Missing positive layer height')
            if not 0 <= tool < len(diameters): raise ValueError('No diameter for selected tool')
            width = forced_width
            if width <= 0:
                area = delta_e * math.pi * (diameters[tool]/2)**2 / distance
                width = area * 1.05**2 / height if role == 'Outer wall' else area/height + (1-math.pi/4)*height
            width = min(width, max(2., 4*height))
            if not math.isfinite(width) or width <= 0: raise ValueError('Invalid extrusion width')
            rows.append([*before, *xyz, width, height, tool]); roles[role] += 1
            layers[layer].add(tool); layer_z[layer] = float(xyz[2])
    if not rows: raise ValueError('No marked model surface extrusion found')
    paths = np.asarray(rows, dtype='<f4')
    tools = sorted({t for group in layers.values() for t in group})
    # Repeated appearances of each tool measure the actual color-cycle pitch.
    pitches = []
    for t in tools:
        zs = [layer_z[k] for k in sorted(layers) if t in layers[k]]
        pitches.extend(np.diff(zs).tolist())
    kept = ('estimated printing time (normal mode)', 'total filament used [g]',
            'filament used [g]', 'filament_colour', 'layer_height', 'nozzle_diameter',
            'printer_model', 'single_extruder_multi_material')
    stats = {'segments': len(paths), 'surface_roles': dict(roles), 'layer_markers': layer,
             'surface_layers': len(layers), 'tools': tools,
             'tool_commands': dict(Counter(tool_commands)),
             'tool_changes': sum(a != b for a, b in zip(tool_commands, tool_commands[1:])),
             'one_surface_tool_per_layer': all(len(v) == 1 for v in layers.values()),
             'color_cycle_median_mm': float(np.median(pitches)) if pitches else None,
             'width_percentiles_mm': np.percentile(paths[:,6], [0, 5, 50, 95, 100]).tolist(),
             'bounds': [np.minimum(paths[:,:3], paths[:,3:6]).min(0).tolist(),
                        np.maximum(paths[:,:3], paths[:,3:6]).max(0).tolist()],
             'ignored_non_surface_arcs': dict(unsupported),
             'metadata': {k: metadata[k] for k in kept if k in metadata}}
    return paths, stats


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('baseline', type=Path); parser.add_argument('candidate', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=False)
    reports = []
    for name, path in [('baseline', args.baseline), ('candidate', args.candidate)]:
        raw = path.read_bytes(); paths, stats = parse_gcode(raw.decode('utf-8'))
        binary = paths.tobytes(); (args.output/f'{name}.bin').write_bytes(binary)
        reports.append({'name': name, 'source': str(path.resolve()),
                        'gcode_sha256': hashlib.sha256(raw).hexdigest(),
                        'binary_sha256': hashlib.sha256(binary).hexdigest(), **stats})
    palette = reports[0]['metadata']['filament_colour'].split(';')
    if any(r['metadata']['filament_colour'].split(';') != palette for r in reports):
        raise ValueError('Comparison palettes differ')
    if any(max(r['tools']) >= len(palette) for r in reports): raise ValueError('Invalid palette index')
    report = {'variants': reports, 'palette': palette, 'stride': 9,
              'renderer': 'Rectangular model-surface bead approximation from actual G-code; no optical averaging, TD or material simulation.',
              'physical_print': 'NOT_RUN', 'hardware_mapping': 'NOT_VALIDATED',
              'implementation_sha256': {p:hashlib.sha256(Path(__file__).with_name(p).read_bytes()).hexdigest()
                  for p in ['imagemap_toolpath_compare.py','imagemap_toolpath_compare.html']}}
    (args.output/'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    (args.output/'index.html').write_text(Path(__file__).with_suffix('.html').read_text(encoding='utf-8'), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__': main()
