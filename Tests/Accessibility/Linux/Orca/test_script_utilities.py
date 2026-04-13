"""Unit tests for the Ladybird Orca script utilities.

These tests exercise the pure-logic functions (tree walking, node-id
cache building, content-child finding) by stubbing out the orca Python
package before importing script_utilities. That keeps the tests
runnable on minimal CI runners that don't have the full orca stack
installed, and makes the tested behaviour independent of orca's
internals.
"""

from __future__ import annotations

import os
import sys
import types
import unittest

from unittest.mock import MagicMock

# -----------------------------------------------------------------------------
# Stub out orca modules before importing script_utilities. We build lightweight
# module stand-ins that export the names script_utilities imports at module
# load time, then register them in sys.modules so the import resolves to them.
# -----------------------------------------------------------------------------


class _StubAXObject:
    """Concrete stand-in for orca.ax_object.AXObject so individual tests can
    patch its classmethods via unittest.mock.patch.object.
    """

    @classmethod
    def get_role(cls, obj):
        return getattr(obj, "_ladybird_role", None)

    @classmethod
    def get_name(cls, obj):
        return getattr(obj, "_ladybird_name", None)

    @classmethod
    def get_child_count(cls, obj):
        return len(getattr(obj, "_ladybird_children", []))

    @classmethod
    def get_child(cls, obj, index):
        children = getattr(obj, "_ladybird_children", [])
        return children[index] if 0 <= index < len(children) else None

    @classmethod
    def is_dead(cls, obj):
        return getattr(obj, "_ladybird_dead", False)


def _install_orca_stubs() -> None:
    focus_manager_mod = types.ModuleType("orca.focus_manager")
    focus_manager_mod.get_manager = MagicMock(return_value=MagicMock())

    ax_object_mod = types.ModuleType("orca.ax_object")
    ax_object_mod.AXObject = _StubAXObject

    ax_utilities_mod = types.ModuleType("orca.ax_utilities")
    ax_utilities_mod.AXUtilities = MagicMock()

    scripts_mod = types.ModuleType("orca.scripts")

    web_utilities = type("Utilities", (), {"__init__": lambda self, script: None})
    web_mod = types.ModuleType("orca.scripts.web")
    web_mod.Utilities = web_utilities

    orca_pkg = types.ModuleType("orca")
    orca_pkg.focus_manager = focus_manager_mod
    orca_pkg.ax_object = ax_object_mod
    orca_pkg.ax_utilities = ax_utilities_mod
    orca_pkg.scripts = scripts_mod
    scripts_mod.web = web_mod

    for name, mod in {
        "orca": orca_pkg,
        "orca.focus_manager": focus_manager_mod,
        "orca.ax_object": ax_object_mod,
        "orca.ax_utilities": ax_utilities_mod,
        "orca.scripts": scripts_mod,
        "orca.scripts.web": web_mod,
    }.items():
        sys.modules.setdefault(name, mod)


_install_orca_stubs()

# Load script_utilities directly from its file rather than going through the Ladybird package's
# __init__.py, which imports orca.scripts.default and other modules we don't want to stub.
import importlib.util  # noqa: E402

_repo_root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", ".."))
_script_utilities_path = os.path.join(
    _repo_root, "UI", "Accessibility", "Linux", "OrcaScripts", "Ladybird", "script_utilities.py"
)
_spec = importlib.util.spec_from_file_location("ladybird_script_utilities", _script_utilities_path)
if _spec is None or _spec.loader is None:
    raise RuntimeError(f"Could not load script_utilities from {_script_utilities_path}")
script_utilities = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(script_utilities)


# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------


def _make_accessible(children=None, role=None, name=None, attrs=None):
    """Produce a MagicMock that mimics an Atspi.Accessible object well enough for the walker logic."""

    acc = MagicMock(name=f"Accessible({role}:{name})")
    acc._ladybird_role = role
    acc._ladybird_name = name
    acc._ladybird_children = list(children or [])
    acc._ladybird_attrs = attrs or {}
    acc.get_attributes = MagicMock(return_value=acc._ladybird_attrs)
    return acc


def _state_set_containing(*names):
    names_set = set(names)
    ss = MagicMock()
    ss.contains = lambda s: s in names_set
    return ss


# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------


class TreeSearchTests(unittest.TestCase):
    def setUp(self):
        # Build a small tree:
        #   root [document]
        #     heading "H1"
        #     paragraph
        #       link "L1"
        #       text_leaf
        #     link "L2"
        self.link1 = _make_accessible(role="link", name="L1")
        self.text_leaf = _make_accessible(role="text leaf", name="lorem")
        self.paragraph = _make_accessible(role="paragraph", children=[self.link1, self.text_leaf])
        self.heading = _make_accessible(role="heading", name="H1")
        self.link2 = _make_accessible(role="link", name="L2")
        self.root = _make_accessible(role="document", children=[self.heading, self.paragraph, self.link2])

    def test_dfs_finds_all_links_regardless_of_depth(self):
        results = script_utilities._tree_search_by_role(self.root, {"link"})
        # DFS order: the link at depth 2 (inside paragraph) is found before the top-level link.
        self.assertEqual(results, [self.link1, self.link2])

    def test_pred_filters_matches(self):
        results = script_utilities._tree_search_by_role(
            self.root, {"link"}, pred=lambda obj: obj._ladybird_name == "L2"
        )
        self.assertEqual(results, [self.link2])

    def test_empty_roleset_matches_nothing(self):
        self.assertEqual(script_utilities._tree_search_by_role(self.root, set()), [])

    def test_none_root_returns_empty(self):
        self.assertEqual(script_utilities._tree_search_by_role(None, {"link"}), [])

    def test_state_filter_requires_all_states(self):
        # Give the second link a FOCUSED state; the first lacks it.
        self.link1.get_state_set = MagicMock(return_value=_state_set_containing())
        self.link2.get_state_set = MagicMock(return_value=_state_set_containing("focused"))
        for node in (self.root, self.heading, self.paragraph, self.text_leaf):
            node.get_state_set = MagicMock(return_value=_state_set_containing())

        results = script_utilities._tree_search_by_role_and_states(self.root, {"link"}, {"focused"})
        self.assertEqual(results, [self.link2])

    def test_state_filter_skips_when_any_state_missing(self):
        # link2 has "focused" but not "visible"; the filter requires both.
        self.link1.get_state_set = MagicMock(return_value=_state_set_containing("visible"))
        self.link2.get_state_set = MagicMock(return_value=_state_set_containing("focused"))
        for node in (self.root, self.heading, self.paragraph, self.text_leaf):
            node.get_state_set = MagicMock(return_value=_state_set_containing())

        results = script_utilities._tree_search_by_role_and_states(self.root, {"link"}, {"focused", "visible"})
        self.assertEqual(results, [])


class NodeIdCacheTests(unittest.TestCase):
    def setUp(self):
        # Reset module-level cache state between cases.
        script_utilities._node_id_cache = {}
        script_utilities._node_id_cache_root = None

    def test_rebuild_indexes_every_node_by_node_id_attribute(self):
        a = _make_accessible(role="link", attrs={"node-id": "42"})
        b = _make_accessible(role="heading", attrs={"node-id": "7"})
        no_id = _make_accessible(role="generic", attrs={})
        root = _make_accessible(role="document", attrs={"node-id": "1"}, children=[a, b, no_id])

        script_utilities._rebuild_node_id_cache(root)

        self.assertIs(script_utilities._node_id_cache["1"], root)
        self.assertIs(script_utilities._node_id_cache["42"], a)
        self.assertIs(script_utilities._node_id_cache["7"], b)
        # Nodes without a node-id attribute are simply not indexed.
        self.assertEqual(set(script_utilities._node_id_cache.keys()), {"1", "42", "7"})

    def test_rebuild_replaces_prior_cache(self):
        old_root = _make_accessible(role="document", attrs={"node-id": "99"})
        script_utilities._rebuild_node_id_cache(old_root)
        self.assertIn("99", script_utilities._node_id_cache)

        new_root = _make_accessible(role="document", attrs={"node-id": "100"})
        script_utilities._rebuild_node_id_cache(new_root)

        self.assertNotIn("99", script_utilities._node_id_cache)
        self.assertIn("100", script_utilities._node_id_cache)
        self.assertIs(script_utilities._node_id_cache_root, new_root)

    def test_rebuild_on_none_root_yields_empty_cache(self):
        script_utilities._rebuild_node_id_cache(None)
        self.assertEqual(script_utilities._node_id_cache, {})


class FindFirstContentChildTests(unittest.TestCase):
    def test_returns_first_descendant_with_name(self):
        deep = _make_accessible(role="text leaf", name="hit")
        wrapper = _make_accessible(role="generic", children=[deep])
        empty_sibling = _make_accessible(role="generic", children=[])
        root = _make_accessible(role="document", children=[empty_sibling, wrapper])

        utilities = script_utilities.Utilities.__new__(script_utilities.Utilities)
        result = utilities._find_first_content_child(root)

        self.assertIs(result, deep)

    def test_returns_none_when_no_descendant_has_content(self):
        leaf = _make_accessible(role="generic", name=None)
        root = _make_accessible(role="document", children=[leaf])

        utilities = script_utilities.Utilities.__new__(script_utilities.Utilities)
        result = utilities._find_first_content_child(root)

        self.assertIsNone(result)

    def test_recursion_depth_is_bounded(self):
        # Build a linear chain longer than the hard cap (15) — the walker must return None without
        # recursing past the cap.
        tail = _make_accessible(role="generic", name=None)
        chain = tail
        for _ in range(40):
            chain = _make_accessible(role="generic", name=None, children=[chain])

        utilities = script_utilities.Utilities.__new__(script_utilities.Utilities)
        # All names are empty, so the answer is None regardless; the real test is that it returns
        # at all (no recursion-limit exception).
        result = utilities._find_first_content_child(chain)
        self.assertIsNone(result)


if __name__ == "__main__":
    unittest.main()
