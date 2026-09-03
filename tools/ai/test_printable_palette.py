#!/usr/bin/env python3
import unittest

from tools.ai import printable_palette as palette


class PrintablePaletteTests(unittest.TestCase):
    def test_normalizes_requested_color_count_with_legacy_default(self):
        self.assertEqual(palette.normalize_palette_color_count(None), 4)
        self.assertEqual(palette.normalize_palette_color_count(" 6 "), 6)
        for value in (True, 0, 7, "six"):
            with self.subTest(value=value), self.assertRaises(palette.PrintablePaletteError):
                palette.normalize_palette_color_count(value)

    def test_normalizes_unique_colors_for_every_supported_cardinality(self):
        self.assertEqual(palette.normalize_palette(["#aa0000", "#AA0000", "#00bb00"]), ("#AA0000", "#00BB00"))
        colors = tuple(f"#{index:06X}" for index in range(1, 7))
        for count in range(palette.MIN_PRINTABLE_COLORS, palette.MAX_PRINTABLE_COLORS + 1):
            with self.subTest(count=count):
                self.assertEqual(palette.normalize_palette(colors[:count]), colors[:count])
        with self.assertRaises(palette.PrintablePaletteError):
            palette.normalize_palette((*colors, "#000007"))

    def test_assigns_every_color_one_deterministic_active_role(self):
        colors = ("#ED6A5A", "#20232A", "#F4F1DE", "#3D5A80", "#2A9D8F", "#9B5DE5")
        for count in range(palette.MIN_PRINTABLE_COLORS, palette.MAX_PRINTABLE_COLORS + 1):
            with self.subTest(count=count):
                result = palette.assign_palette_roles(colors[:count])
                self.assertEqual(tuple(result.color_by_role), palette.active_palette_roles(count))
                self.assertEqual(set(result.role_by_color), set(colors[:count]))
                self.assertEqual(len(set(result.color_by_role.values())), count)
                self.assertEqual(result, palette.assign_palette_roles(colors[:count]))

    def test_assigns_arbitrary_four_colors_without_rgbw_assumptions(self):
        result = palette.assign_palette_roles(("#F28C28", "#6A0DAD", "#111111", "#F4D03F"))
        self.assertEqual(result.color_by_role["structure"], "#111111")
        self.assertEqual(result.color_by_role["light"], "#F4D03F")
        self.assertEqual(set(result.role_by_color), set(result.palette))
        self.assertEqual(set(result.color_by_role), set(palette.active_palette_roles(4)))

    def test_five_and_six_colors_receive_extended_semantic_roles(self):
        colors = ("#ED6A5A", "#20232A", "#F4F1DE", "#3D5A80", "#2A9D8F", "#9B5DE5")
        five = palette.assign_palette_roles(colors[:5])
        six = palette.assign_palette_roles(colors)
        self.assertIn("secondary", five.color_by_role)
        self.assertNotIn("detail", five.color_by_role)
        self.assertIn("detail", six.color_by_role)

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
