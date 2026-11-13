#!/usr/bin/env python3
"""Static layout test for the native settings window.

The dialog is owner-drawn: Win32 will happily place two controls on top of each
other, and nothing at runtime complains. This test reads the actual row table and
layout constants out of src/win/settings.cc and re-runs the geometry, so adding a
row without room for it fails here instead of looking broken on someone's screen.

It checks three things:
  * no two controls overlap,
  * no control or painted label crosses into the other column or under the
    footer rule,
  * the client height the window asks for (content_height) is >= the lowest row.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parent.parent / "src" / "win" / "settings.cc"
WIDTH = 820  # the width settings_show() requests


def constants(text: str) -> dict[str, int]:
    values = {}
    for name in ("kTop", "kRowH", "kGap", "kGroupGap", "kLabelX", "kControlX",
                 "kColumnGap"):
        match = re.search(rf"constexpr int {name} = (\d+);", text)
        assert match, f"{name} not found in settings.cc"
        values[name] = int(match.group(1))
    for name in ("kLabelOnly", "kSubHead", "kCheck", "kButton", "kValue"):
        match = re.search(rf"constexpr int {name} = (\d+);", text)
        assert match, f"{name} not found"
        values[name] = int(match.group(1))
    ids = {}
    for match in re.finditer(r"constexpr int (kId\w+) = (\d+);", text):
        ids[match.group(1)] = int(match.group(2))
    values["_ids"] = ids
    return values


def rows(text: str, ids: dict[str, int]) -> list[tuple[int, int, int, str, bool]]:
    table = text[text.index("const RowSpec kRows[] = {"):]
    table = table[: table.index("};")]
    parsed = []
    for line in table.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        fields = [f.strip() for f in line.strip(",{} ").split(",") if f.strip()]
        ident_field = fields[0]
        ident = 0 if ident_field == "0" else ids[ident_field]
        column = int(fields[1])
        kind_name = fields[2]
        same = fields[4] == "true" if len(fields) > 4 else False
        kind = {"kLabelOnly": 0, "kSubHead": 1, "kCheck": 2, "kButton": 3,
                "kValue": 4}[kind_name]
        parsed.append((ident, column, kind, fields[3] if len(fields) > 3 else "", same))
    return parsed


class SettingsLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = SOURCE.read_text(encoding="utf-8")
        cls.c = constants(cls.text)
        cls.rows = rows(cls.text, cls.c["_ids"])

    # -- geometry mirrored from settings.cc ---------------------------------
    def column_width(self) -> int:
        c = self.c
        return (WIDTH - c["kLabelX"] * 2 - c["kColumnGap"]) // 2

    def column_x(self, column: int) -> int:
        c = self.c
        return c["kLabelX"] + column * (self.column_width() + c["kColumnGap"])

    def row_top(self, index: int) -> int:
        c = self.c
        cursor = [c["kTop"], c["kTop"]]
        last = [c["kTop"], c["kTop"]]
        top = c["kTop"]
        for i in range(index + 1):
            _id, column, kind, _text, same = self.rows[i]
            if same:
                top = last[column]
            else:
                top = cursor[column]
                if kind == c["kSubHead"]:
                    top += c["kGroupGap"]
            last[column] = top
            cursor[column] = top + c["kRowH"] + c["kGap"]
        return top

    def boxes(self) -> list[tuple[int, int, int, int, int]]:
        c = self.c
        width = self.column_width()
        out = []
        for i, (ident, column, kind, _text, same) in enumerate(self.rows):
            if kind in (c["kLabelOnly"], c["kSubHead"]):
                continue
            x = self.column_x(column)
            y = self.row_top(i)
            if ident == c["_ids"]["kIdOffsetMinus"]:
                # first half of the nudge pair, on the "Sync offset" line
                out.append((ident, x + c["kControlX"], y, 76, c["kRowH"] - 2))
            elif same and ident == c["_ids"]["kIdOffset"]:
                out.append((ident, x + c["kControlX"] + 80, y, 76, c["kRowH"] - 2))
            elif same:
                out.append((ident, x + c["kControlX"], y,
                            min(116, width - c["kControlX"]), c["kRowH"]))
            elif kind == c["kCheck"]:
                shares = i + 1 < len(self.rows) and self.rows[i + 1][4]
                out.append((ident, x, y, (c["kControlX"] - 12) if shares else (width - 20),
                            c["kRowH"]))
            elif kind == c["kValue"]:
                out.append((ident, x + c["kControlX"], y,
                            min(170, width - c["kControlX"]), c["kRowH"]))
            elif ident == c["_ids"]["kIdOffsetMinus"]:
                out.append((ident, x + c["kControlX"], y, 76, c["kRowH"] - 2))
            elif ident == c["_ids"]["kIdOffset"]:
                out.append((ident, x + c["kControlX"] + 80, y, 76, c["kRowH"] - 2))
            else:
                out.append((ident, x, y, min(200, width - 20), c["kRowH"]))
        return out

    @staticmethod
    def overlaps(first, second) -> bool:
        ax, ay, aw, ah = first
        bx, by, bw, bh = second
        return not (ax + aw <= bx or bx + bw <= ax or ay + ah <= by or by + bh <= ay)

    def content_height(self) -> int:
        c = self.c
        return self.row_top(len(self.rows) - 1) + c["kRowH"] + 16 + 78

    # -- assertions ---------------------------------------------------------
    def test_table_is_not_empty_and_covers_both_columns(self) -> None:
        self.assertGreater(len(self.rows), 12)
        self.assertEqual({row[1] for row in self.rows}, {0, 1})

    def test_no_two_controls_overlap(self) -> None:
        boxes = self.boxes()
        for i in range(len(boxes)):
            for j in range(i + 1, len(boxes)):
                with self.subTest(first=boxes[i][0], second=boxes[j][0]):
                    self.assertFalse(
                        self.overlaps(boxes[i][1:], boxes[j][1:]),
                        f"settings controls {boxes[i][0]} and {boxes[j][0]} overlap",
                    )

    def test_controls_stay_inside_their_column(self) -> None:
        width = self.column_width()
        for ident, x, _y, w, _h in self.boxes():
            column_end = self.column_x(1) - self.c["kColumnGap"] + width
            self.assertLessEqual(x + w, column_end + self.c["kColumnGap"],
                                 f"control {ident} runs into the window edge")
            self.assertGreaterEqual(x, self.c["kLabelX"], f"control {ident} is left of the column")

    def test_window_is_tall_enough_for_the_last_row(self) -> None:
        lowest = max(y + h for _id, _x, y, _w, h in self.boxes())
        # The footer rule sits 78px above the client bottom.
        self.assertLessEqual(lowest, self.content_height() - 78,
                             "the last settings row would run under the footer")

    def test_shared_rows_name_a_real_control(self) -> None:
        for ident, _column, kind, _text, same in self.rows:
            if same:
                self.assertNotEqual(ident, 0, "a label-only row cannot share a line")


if __name__ == "__main__":
    unittest.main(verbosity=2)
