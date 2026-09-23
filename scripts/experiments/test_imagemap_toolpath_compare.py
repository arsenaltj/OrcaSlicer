import math
import unittest
import numpy as np
from imagemap_toolpath_compare import parse_gcode


PREFIX = '''G90
M83
G1 X0 Y0 Z0.2
;LAYER_CHANGE
;HEIGHT:0.2
; printing object sample id:0 copy 0
;TYPE:Outer wall
;WIDTH:0
'''
META = '\n; filament_diameter = 1.75,1.75\n'


class ToolpathTests(unittest.TestCase):
    def test_relative_extrusion_width_and_tower_exclusion(self):
        p,s = parse_gcode(PREFIX+'G1 X1 E0.03\n; stop printing object sample\n;TYPE:Prime tower\nG1 X2 E1'+META)
        self.assertEqual(len(p),1)
        self.assertAlmostEqual(float(p[0,6]),.03*math.pi*(1.75/2)**2*1.05**2/.2,places=6)
        self.assertEqual(s['surface_roles'],{'Outer wall':1})

    def test_absolute_extrusion_retract_reset_and_explicit_width(self):
        g=PREFIX.replace('M83','M82').replace(';WIDTH:0',';WIDTH:0.4')
        p,_=parse_gcode(g+'G92 E5\nG1 X1 E5.1\nG1 E4.9\nG1 E5.1\nG92 E0\nG1 X2 E0.1'+META)
        self.assertEqual(len(p),2)
        np.testing.assert_allclose(p[:,6],.4)

    def test_relative_coordinates_and_same_tool_cycle(self):
        g=PREFIX+'T0\nG1 X1 E0.02\n;LAYER_CHANGE\nT1\nG1 Z0.4\nG1 X2 E0.02\n;LAYER_CHANGE\nT0\nG1 Z0.6\nG91\nG1 X1 E0.02'
        p,s=parse_gcode(g+META)
        self.assertEqual(p[-1,3],3)
        self.assertAlmostEqual(s['color_cycle_median_mm'],.4)
        self.assertEqual(s['tool_changes'],2)
        self.assertTrue(s['one_surface_tool_per_layer'])

    def test_unsupported_surface_motion_fails_closed(self):
        for body in ['G2 X1 Y1 I0 J1 E0.1','G1 X1 Z0.4 E0.1','G20\nG1 X1 E0.1']:
            with self.subTest(body=body),self.assertRaises(ValueError):parse_gcode(PREFIX+body+META)
        with self.assertRaises(ValueError):parse_gcode(PREFIX.replace('M83\n','')+'G1 X1 E0.1'+META)


if __name__ == '__main__': unittest.main()
