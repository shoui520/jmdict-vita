from __future__ import annotations

import gzip
from pathlib import Path
import tempfile
import unittest

from tools.convert_edrdg import DictionaryReader, build_dictionary, normalize_key, normalize_query


JMDICT_XML = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE JMdict [
<!ENTITY n "noun (common)">
]>
<JMdict>
  <entry>
    <ent_seq>1000001</ent_seq>
    <k_ele><keb>猫</keb></k_ele>
    <r_ele><reb>ネコ</reb></r_ele>
    <sense>
      <pos>&n;</pos>
      <gloss>cat</gloss>
      <gloss>domestic cat</gloss>
      <example><ex_sent xml:lang="jpn">猫がいる。</ex_sent><ex_sent xml:lang="eng">There is a cat.</ex_sent></example>
    </sense>
  </entry>
  <entry>
    <ent_seq>1000002</ent_seq>
    <r_ele><reb>たべる</reb></r_ele>
    <sense><gloss>to eat</gloss></sense>
  </entry>
  <entry>
    <ent_seq>1000003</ent_seq>
    <k_ele><keb>太郎</keb></k_ele>
    <r_ele><reb>たろう</reb></r_ele>
    <sense><gloss>first son</gloss></sense>
  </entry>
</JMdict>
"""

JMNEDICT_XML = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE JMnedict [<!ENTITY masc "male given name">]>
<JMnedict>
  <entry>
    <ent_seq>2000001</ent_seq>
    <k_ele><keb>太郎</keb></k_ele>
    <r_ele><reb>たろう</reb></r_ele>
    <trans><name_type>&masc;</name_type><trans_det>Tarou</trans_det><trans_det>Taro</trans_det></trans>
  </entry>
</JMnedict>
"""


class ConverterTests(unittest.TestCase):
    def test_normalization_matches_runtime_contract(self) -> None:
        self.assertEqual(normalize_key(" ネコ "), "ねこ")
        self.assertEqual(normalize_key("ＣＡＴ!"), "cat")
        self.assertEqual(normalize_key("ice cream"), "icecream")
        self.assertEqual(normalize_key("ﾈｺ"), "ﾈｺ")
        self.assertEqual(normalize_query("ice cream"), "ice cream")
        self.assertEqual(normalize_query(" ネコ "), "ねこ")

    def test_stream_build_and_prefix_lookup(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jmdict = root / "JMdict.xml.gz"
            jmnedict = root / "JMnedict.xml"
            output = root / "dictionary.bin"
            with gzip.open(jmdict, "wb") as handle:
                handle.write(JMDICT_XML.encode("utf-8"))
            jmnedict.write_text(JMNEDICT_XML, encoding="utf-8")

            report = build_dictionary(output, jmdict, jmnedict, progress_every=0)
            self.assertEqual(report["jmdict_entries"], 3)
            self.assertEqual(report["jmnedict_entries"], 1)
            self.assertGreater(report["index_associations"], 6)
            self.assertGreater(report["unique_keys"], 6)
            self.assertLessEqual(report["max_entry_bytes"], 48 * 1024)

            reader = DictionaryReader(output)
            try:
                self.assertIn("猫", reader.search("猫")[0])
                self.assertIn("猫", reader.search("ねこ")[0])
                self.assertIn("猫", reader.search("cat")[0])
                self.assertIn("猫", reader.search("domestic cat")[0])
                self.assertIn("たべる", reader.search("eat")[0])
                self.assertIn("太郎", reader.search("Taro")[0])
                self.assertIn("JMdict", reader.search("太郎")[0])
                self.assertIn("JMnedict", reader.search("太郎")[1])
                self.assertIn("entry 1000001", reader.search("猫")[0])
                self.assertNotIn("There is a cat", reader.search("猫")[0])
                self.assertEqual(reader.search("missing"), [])
            finally:
                reader.close()

    def test_crosses_key_and_entry_block_boundaries(self) -> None:
        entries = []
        padding = "descriptive " * 80
        for index in range(140):
            entries.append(
                f"<entry><ent_seq>{3000000 + index}</ent_seq>"
                f"<k_ele><keb>word{index:03d}</keb></k_ele>"
                f"<r_ele><reb>わーど{index:03d}</reb></r_ele>"
                f"<sense><gloss>unique{index:03d} {padding}</gloss></sense></entry>"
            )
        document = ("<JMdict>" + "".join(entries) + "</JMdict>").encode("utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "JMdict.xml.gz"
            output = root / "dictionary.bin"
            with gzip.open(source, "wb") as handle:
                handle.write(document)
            report = build_dictionary(output, source, None, progress_every=0)
            self.assertGreater(report["key_blocks"], 1)
            self.assertGreater(report["entry_blocks"], 1)
            reader = DictionaryReader(output)
            try:
                self.assertIn("entry 3000000", reader.search("word000")[0])
                self.assertIn("entry 3000139", reader.search("unique139")[0])
            finally:
                reader.close()


if __name__ == "__main__":
    unittest.main()
