#!/usr/bin/env python3
"""Keep GKI exports separate when the Android output path contains onyx."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build import output_category


class ExportPaths(unittest.TestCase):
    def test_android_product_directory_does_not_classify_outputs(self):
        root = Path('/rom/out/target/product/onyx/obj/kernel/execroot/_main')
        for name in ('Image', 'kernel_aarch64_Module.symvers', 'modules.builtin'):
            with self.subTest(name=name):
                path = root / 'bazel-out/k8-fastbuild/bin/common/kernel_aarch64' / name
                self.assertEqual(output_category(path, root), 'gki')
        path = root / 'bazel-out/k8-fastbuild/bin/onyx/wlan/wlan.ko'
        self.assertEqual(output_category(path, root), 'platform')

    def test_outputs_outside_execution_root_are_rejected(self):
        with self.assertRaises(ValueError):
            output_category(Path('/other/Image'), Path('/rom/execroot/_main'))


if __name__ == '__main__':
    unittest.main()
