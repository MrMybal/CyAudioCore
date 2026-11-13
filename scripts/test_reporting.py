"""Deterministic input-fingerprint checks; no native compilation or remote access."""
import json
from pathlib import Path
import tempfile
import unittest
from reporting import dependency_fingerprint, source_digest


class Fingerprints(unittest.TestCase):
    def test_dependency_content_and_archive_metadata(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'third_party').mkdir()
            (root/'include').mkdir()
            (root/'src').mkdir()
            (root/'CMakeLists.txt').write_text('project(Fixture)\n', encoding='utf-8')
            (root/'third_party/manifest.json').write_text(json.dumps({'components': []}), encoding='utf-8')
            (root/'include/A.h').write_text('struct A {};\n', encoding='utf-8')
            first = dependency_fingerprint(root)
            self.assertIsNone(first['revision'])
            self.assertIsNone(first['trackedChanges'])
            (root/'README.md').write_text('A documentation edit\n', encoding='utf-8')
            self.assertEqual(first['sourceSha256'], source_digest(root))
            (root/'include/A.h').write_text('struct A { int value; };\n', encoding='utf-8')
            second = source_digest(root)
            self.assertNotEqual(first['sourceSha256'], second)
            (root/'src/New.cpp').write_text('int added() { return 0; }\n', encoding='utf-8')
            self.assertNotEqual(second, source_digest(root))


if __name__ == '__main__':
    unittest.main()
