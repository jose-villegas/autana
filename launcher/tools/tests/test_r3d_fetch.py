"""Source caches publish only complete archives and extracted trees."""
import hashlib
import pathlib
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d import fetch


class FetchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.archive = self.root / 'source.zip'
        with zipfile.ZipFile(self.archive, 'w') as archive:
            archive.writestr('model/mesh.obj', 'complete mesh')
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.cache = self.root / 'cache'
        cache = patch.object(fetch, 'CACHE', self.cache)
        cache.start()
        self.addCleanup(cache.stop)

    def test_extraction_is_private_until_complete(self):
        original = zipfile.ZipFile.extractall
        def extract(archive, destination, *args, **kwargs):
            self.assertFalse((self.cache / 'model').exists())
            self.assertNotEqual(pathlib.Path(destination), self.cache / 'model')
            return original(archive, destination, *args, **kwargs)
        with patch.object(zipfile.ZipFile, 'extractall', extract):
            result = fetch.fetch_zip(self.archive.as_uri(), self.digest, 'model')
        self.assertEqual((result / 'model/mesh.obj').read_text(), 'complete mesh')
        self.assertEqual(set(self.cache.iterdir()), {self.cache / 'model.zip', self.cache / 'model'})

    def test_losing_publication_race_returns_complete_winner(self):
        def replace(staged, destination):
            destination.mkdir()
            (destination / 'winner').write_text('complete')
            raise FileExistsError('another process published first')
        self.cache.mkdir()
        (self.cache / 'model.zip').write_bytes(self.archive.read_bytes())
        with patch.object(fetch.os, 'replace', side_effect=replace):
            result = fetch.fetch_zip(self.archive.as_uri(), self.digest, 'model')
        self.assertEqual((result / 'winner').read_text(), 'complete')
        self.assertEqual(set(self.cache.iterdir()), {self.cache / 'model.zip', self.cache / 'model'})
