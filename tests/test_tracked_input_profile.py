import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class TrackedInputProfileTest(unittest.TestCase):
    def test_native_dashboard_bindings_use_declared_inputs_and_stock_actions(self):
        directory = ROOT / "driver/resources/input"
        profile = json.loads((directory / "sc26_profile.json").read_text())
        binding = json.loads((directory / "vrcompositor_bindings_sc26.json").read_text())
        stock = json.loads((ROOT / "tests/fixtures/compositor-action-contract.json").read_text())
        self.assertEqual(profile["controller_type"], binding["controller_type"])
        self.assertNotEqual(profile["controller_type"], stock["controller_type"])
        self.assertEqual(
            profile["default_bindings"],
            [{"app_key": stock["app_key"], "binding_url": "vrcompositor_bindings_sc26.json"}],
        )
        outputs = set()
        for action_set, group in binding["bindings"].items():
            self.assertIn(action_set, stock["bindings"])
            stock_outputs = {
                value["output"].lower()
                for source in stock["bindings"][action_set].get("sources", [])
                for value in source.get("inputs", {}).values()
            }
            stock_outputs.update(
                pose["output"].lower() for pose in stock["bindings"][action_set].get("poses", [])
            )
            for source in [*group.get("poses", []), *group.get("sources", [])]:
                self.assertTrue(source["path"].startswith("/user/hand/right/"))
                path = source["path"].removeprefix("/user/hand/right")
                self.assertIn(path, profile["input_source"])
                targets = (
                    [source["output"]]
                    if "output" in source
                    else [value["output"] for value in source["inputs"].values()]
                )
                for target in targets:
                    self.assertIn(target.lower(), stock_outputs)
                    outputs.add(target.lower())
        self.assertTrue(
            {
                "/actions/lasermouse/in/pointer",
                "/actions/lasermouse/in/leftclick",
                "/actions/scroll_smooth/in/scroll",
                "/actions/lasermouse_secondary/in/switchlaserhand",
            }
            <= outputs
        )
        self.assertNotIn("/input/system", profile["input_source"])
        self.assertEqual(
            binding["bindings"]["/actions/lasermouse"]["poses"],
            [{"output": "/actions/lasermouse/in/Pointer", "path": "/user/hand/right/pose/cursor"}],
        )

    def test_activation_selects_laser_without_click_or_scroll(self):
        groups = json.loads(
            (ROOT / "driver/resources/input/vrcompositor_bindings_sc26.json").read_text()
        )["bindings"]
        outputs = [
            value["output"]
            for group in groups.values()
            for source in group.get("sources", [])
            if source["path"].endswith("/input/activate")
            for value in source["inputs"].values()
        ]
        self.assertEqual(outputs, ["/actions/lasermouse_secondary/in/switchlaserhand"])
        self.assertEqual(
            [source["path"] for source in groups["/actions/lasermouse_secondary"]["sources"]],
            ["/user/hand/right/input/activate"],
        )

    def test_existing_pad_trigger_clicks_and_native_scroll_are_preserved(self):
        directory = ROOT / "driver/resources/input"
        profile = json.loads((directory / "sc26_profile.json").read_text())
        groups = json.loads((directory / "vrcompositor_bindings_sc26.json").read_text())["bindings"]

        def paths(group, output):
            return {
                source["path"]
                for source in groups[group]["sources"]
                if any(
                    value["output"].lower().endswith("/" + output.lower())
                    for value in source["inputs"].values()
                )
            }

        prefix = "/user/hand/right/input/"
        self.assertTrue(
            {prefix + "trigger", prefix + "bumper"} <= paths("/actions/lasermouse", "leftclick")
        )
        self.assertTrue(
            {prefix + "grip", prefix + "thumbstick"} <= paths("/actions/lasermouse", "rightclick")
        )
        self.assertIn(prefix + "bumper", paths("/actions/lasermouse", "LockMousePosition"))
        self.assertNotIn(prefix + "cursorpad", paths("/actions/lasermouse", "leftclick"))
        self.assertNotIn(prefix + "trackpad", paths("/actions/lasermouse", "rightclick"))
        for mode in ("smooth", "discrete"):
            self.assertEqual(
                paths("/actions/scroll_" + mode, "scroll"),
                {prefix + "thumbstick", prefix + "trackpad"},
            )
            source = next(
                s
                for s in groups["/actions/scroll_" + mode]["sources"]
                if s["path"].endswith("/trackpad")
            )
            self.assertEqual(
                (source["mode"], source["parameters"]["scroll_mode"]), ("scroll", mode)
            )
        self.assertEqual(profile["input_source"]["/input/trackpad"]["type"], "trackpad")
        self.assertNotIn("/actions/quickmouse", groups)


if __name__ == "__main__":
    unittest.main()
