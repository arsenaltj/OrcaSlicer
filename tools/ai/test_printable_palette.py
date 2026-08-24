#!/usr/bin/env python3
import unittest

from tools.ai import printable_palette as palette


class PrintablePaletteTests(unittest.TestCase):
    def test_normalizes_unique_colors_and_rejects_more_than_four(self):
        self.assertEqual(palette.normalize_palette(["#aa0000", "#AA0000", "#00bb00"]), ("#AA0000", "#00BB00"))
        with self.assertRaises(palette.PrintablePaletteError):
            palette.normalize_palette(["#000000", "#111111", "#222222", "#333333", "#444444"])

    def test_assigns_arbitrary_four_colors_without_rgbw_assumptions(self):
        result = palette.assign_palette_roles(("#F28C28", "#6A0DAD", "#111111", "#F4D03F"))
        self.assertEqual(result.color_by_role["structure"], "#111111")
        self.assertEqual(result.color_by_role["light"], "#F4D03F")
        self.assertEqual(set(result.role_by_color), set(result.palette))
        self.assertEqual(set(result.color_by_role), set(palette.PALETTE_ROLES))

    def test_degrades_to_available_roles(self):
        one = palette.assign_palette_roles(("#336699",))
        self.assertEqual(one.color_by_role, {"primary": "#336699"})
        two = palette.assign_palette_roles(("#111111", "#EEEEEE"))
        self.assertEqual(two.color_by_role, {"structure": "#111111", "primary": "#EEEEEE"})

    def test_manual_override_preserves_one_to_one_roles(self):
        colors = ("#F28C28", "#6A0DAD", "#111111", "#F4D03F")
        result = palette.assign_palette_roles(colors, {"primary": "#6A0DAD", "accent": "#F28C28"})
        self.assertEqual(result.color_by_role["primary"], "#6A0DAD")
        self.assertEqual(result.color_by_role["accent"], "#F28C28")
        self.assertEqual(len(set(result.color_by_role.values())), 4)

    def test_rejects_invalid_manual_role_mapping(self):
        with self.assertRaises(palette.PrintablePaletteError):
            palette.assign_palette_roles(("#000000", "#FFFFFF"), {"accent": "#FFFFFF"})
        with self.assertRaises(palette.PrintablePaletteError):
            palette.assign_palette_roles(
                ("#000000", "#FFFFFF"), {"primary": "#FFFFFF", "structure": "#FFFFFF"}
            )

    def test_warns_when_filament_colors_are_hard_to_distinguish(self):
        close = palette.assign_palette_roles(("#777777", "#7A7A7A", "#7D7D7D", "#808080"))
        separated = palette.assign_palette_roles(("#000000", "#FFFFFF", "#FF0000", "#0066FF"))
        self.assertTrue(close.low_contrast)
        self.assertFalse(separated.low_contrast)


if __name__ == "__main__":
    unittest.main()
