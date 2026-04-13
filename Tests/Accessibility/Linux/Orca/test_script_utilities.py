"""Unit tests for the Ladybird Orca script utilities.

These tests exercise the pure-logic helpers (node-id indexing,
private-bus match encoding, private-bus→Qt-bridge mapping, and the
``active_document`` DFS fallback) by stubbing out the orca Python
package before importing script_utilities. That keeps the tests
runnable on minimal CI runners that don’t have the full orca stack
installed, and makes the tested behaviour independent of orca’s
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
    def get_parent(cls, obj):
        return getattr(obj, "_ladybird_parent", None)

    @classmethod
    def is_dead(cls, obj):
        return getattr(obj, "_ladybird_dead", False)


class _StubAXUtilities:
    """Stand-in for orca.ax_utilities.AXUtilities with a couple of role predicates."""

    @classmethod
    def is_document_web(cls, obj):
        return getattr(obj, "_ladybird_role", None) == "document_web"

    @classmethod
    def is_showing(cls, obj):
        return getattr(obj, "_ladybird_showing", False)


def _install_orca_stubs() -> None:
    focus_manager_mod = types.ModuleType("orca.focus_manager")
    focus_manager_mod.get_manager = MagicMock(return_value=MagicMock())

    ax_object_mod = types.ModuleType("orca.ax_object")
    ax_object_mod.AXObject = _StubAXObject

    ax_utilities_mod = types.ModuleType("orca.ax_utilities")
    ax_utilities_mod.AXUtilities = _StubAXUtilities

    scripts_mod = types.ModuleType("orca.scripts")

    # Class-level ``active_document`` is a plain attribute so individual tests can
    # override it (via ``unittest.mock.patch.object`` or direct assignment) to drive
    # the subclass's override through different ``super()`` outcomes.
    web_utilities = type(
        "Utilities",
        (),
        {
            "__init__": lambda self, script: None,
            "active_document": lambda self: None,
            "clear_cached_objects": lambda self: None,
        },
    )
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


def _make_accessible(children=None, role=None, name=None, attrs=None, showing=False):
    """Produce a MagicMock that mimics an Atspi.Accessible object well enough for the walker logic."""

    acc = MagicMock(name=f"Accessible({role}:{name})")
    acc._ladybird_role = role
    acc._ladybird_name = name
    acc._ladybird_children = list(children or [])
    acc._ladybird_attrs = attrs or {}
    acc._ladybird_showing = showing
    acc._ladybird_parent = None
    # MagicMock auto-creates attributes, so ``is_dead`` would always see a
    # truthy mock unless we explicitly pin the flag to False.
    acc._ladybird_dead = False
    for child in acc._ladybird_children:
        child._ladybird_parent = acc
    acc.get_attributes = MagicMock(return_value=acc._ladybird_attrs)
    return acc


# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------


class NodeIdCacheTests(unittest.TestCase):
    def setUp(self):
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


class MapPrivateBusToQtBridgeTests(unittest.TestCase):
    """``_map_private_bus_to_qt_bridge`` resolves AT-SPI2 object paths from the
    private bus back to Atspi.Accessible objects on the Qt bridge by reading the
    ``node-id`` attribute on each path and looking it up in the cache."""

    def setUp(self):
        script_utilities._node_id_cache = {}
        script_utilities._node_id_cache_root = None
        self._orig_connect = script_utilities._connect_private_bus
        self._orig_name = script_utilities._private_bus_name

    def tearDown(self):
        script_utilities._connect_private_bus = self._orig_connect
        script_utilities._private_bus_name = self._orig_name

    def test_empty_paths_returns_empty_without_calling_the_bus(self):
        # Even if the bus is unavailable, an empty path list is a no-op.
        script_utilities._connect_private_bus = lambda: None
        result = script_utilities._map_private_bus_to_qt_bridge([], qt_root=None)
        self.assertEqual(result, [])

    def test_no_bus_connection_returns_empty(self):
        script_utilities._connect_private_bus = lambda: None
        script_utilities._private_bus_name = None
        result = script_utilities._map_private_bus_to_qt_bridge(["/some/path"], qt_root=None)
        self.assertEqual(result, [])

    def test_resolves_paths_via_node_id_attribute(self):
        # Build two Qt-bridge objects whose node-ids match what the private bus will report.
        a = _make_accessible(role="link", attrs={"node-id": "42"})
        b = _make_accessible(role="heading", attrs={"node-id": "7"})
        qt_root = _make_accessible(role="document", attrs={"node-id": "1"}, children=[a, b])

        # Stand-in D-Bus connection that returns a {node-id: ...} attribute dict per path.
        path_to_node_id = {"/p/a": "42", "/p/b": "7", "/p/missing": "999"}

        class FakeAttrsReply:
            def __init__(self, node_id):
                self._node_id = node_id

            def get_child_value(self, idx):
                assert idx == 0
                return types.SimpleNamespace(unpack=lambda: {"node-id": self._node_id})

        class FakeConn:
            def call_sync(self, _name, path, *_a, **_kw):
                return FakeAttrsReply(path_to_node_id[path])

        fake_conn = FakeConn()
        script_utilities._connect_private_bus = lambda: fake_conn
        script_utilities._private_bus_name = ":1.42"

        result = script_utilities._map_private_bus_to_qt_bridge(["/p/a", "/p/b", "/p/missing"], qt_root=qt_root)

        # Missing node-ids are skipped silently.
        self.assertEqual(result, [a, b])

    def test_skips_dead_qt_objects(self):
        live = _make_accessible(role="link", attrs={"node-id": "live-id"})
        dead = _make_accessible(role="link", attrs={"node-id": "dead-id"})
        dead._ladybird_dead = True
        qt_root = _make_accessible(role="document", children=[live, dead])

        class FakeAttrsReply:
            def __init__(self, node_id):
                self._node_id = node_id

            def get_child_value(self, idx):
                assert idx == 0
                return types.SimpleNamespace(unpack=lambda: {"node-id": self._node_id})

        path_to_node_id = {"/p/live": "live-id", "/p/dead": "dead-id"}

        class FakeConn:
            def call_sync(self, _name, path, *_a, **_kw):
                return FakeAttrsReply(path_to_node_id[path])

        script_utilities._connect_private_bus = lambda: FakeConn()
        script_utilities._private_bus_name = ":1.42"

        result = script_utilities._map_private_bus_to_qt_bridge(["/p/live", "/p/dead"], qt_root=qt_root)
        self.assertEqual(result, [live])


class ActiveDocumentFallbackTests(unittest.TestCase):
    """``Utilities.active_document`` falls back to a window DFS when Orca’s
    default EMBEDS-based path returns ``None`` (which happens on Qt < 6.11
    because Qt’s bridge does not expose the EMBEDS relation)."""

    def setUp(self):
        import orca.scripts.web as web_stub

        self._orig_super_active_document = web_stub.Utilities.active_document
        # Each test sets this to control what ``super().active_document()`` returns.
        self._super_return = None
        web_stub.Utilities.active_document = lambda inner_self: self._super_return

        import orca.focus_manager as fm

        self._orig_get_manager = fm.get_manager
        self._active_window = None

        manager = MagicMock()
        manager.get_active_window = lambda: self._active_window
        fm.get_manager = lambda: manager

    def tearDown(self):
        import orca.scripts.web as web_stub

        web_stub.Utilities.active_document = self._orig_super_active_document

        import orca.focus_manager as fm

        fm.get_manager = self._orig_get_manager

    def _utilities(self):
        # Bypass __init__ (which would try to connect to the private bus and install patches).
        return script_utilities.Utilities.__new__(script_utilities.Utilities)

    def test_super_result_short_circuits(self):
        sentinel = _make_accessible(role="document_web")
        self._super_return = sentinel
        self.assertIs(self._utilities().active_document(), sentinel)

    def test_dfs_prefers_document_whose_parent_is_showing(self):
        # Build a window with two tab panels, each containing a document_web.
        # Only tab B is showing, so its document should win.
        doc_a = _make_accessible(role="document_web")
        panel_a = _make_accessible(role="panel", children=[doc_a], showing=False)
        doc_b = _make_accessible(role="document_web")
        panel_b = _make_accessible(role="panel", children=[doc_b], showing=True)
        self._active_window = _make_accessible(role="frame", children=[panel_a, panel_b])

        self.assertIs(self._utilities().active_document(), doc_b)

    def test_dfs_returns_last_when_no_parent_is_showing(self):
        doc_a = _make_accessible(role="document_web")
        panel_a = _make_accessible(role="panel", children=[doc_a], showing=False)
        doc_b = _make_accessible(role="document_web")
        panel_b = _make_accessible(role="panel", children=[doc_b], showing=False)
        self._active_window = _make_accessible(role="frame", children=[panel_a, panel_b])

        self.assertIs(self._utilities().active_document(), doc_b)

    def test_dfs_returns_none_when_no_document_found(self):
        self._active_window = _make_accessible(role="frame", children=[_make_accessible(role="panel")])

        self.assertIsNone(self._utilities().active_document())

    def test_returns_none_when_no_active_window(self):
        self._active_window = None
        self.assertIsNone(self._utilities().active_document())


class CollectionRoleBitfieldTests(unittest.TestCase):
    """``_private_bus_collection_get_matches`` encodes its role list as an
    AT-SPI2 role bitfield. Validate the encoding shape by capturing the
    Variant tuple passed to ``call_sync``."""

    def setUp(self):
        self._orig_connect = script_utilities._connect_private_bus
        self._orig_name = script_utilities._private_bus_name
        self._orig_doc_root = script_utilities._get_private_bus_doc_root

    def tearDown(self):
        script_utilities._connect_private_bus = self._orig_connect
        script_utilities._private_bus_name = self._orig_name
        script_utilities._get_private_bus_doc_root = self._orig_doc_root

    def _install_capturing_conn(self):
        captured = {}

        class FakeMatches:
            def n_children(self_inner):
                return 0

            def get_child_value(self_inner, idx):
                raise IndexError

        class FakeReply:
            def get_child_value(self_inner, idx):
                assert idx == 0
                return FakeMatches()

        class FakeConn:
            def call_sync(self_inner, name, path, iface, method, args, *_a, **_kw):
                captured["name"] = name
                captured["path"] = path
                captured["iface"] = iface
                captured["method"] = method
                captured["args"] = args
                return FakeReply()

        script_utilities._connect_private_bus = lambda: FakeConn()
        script_utilities._private_bus_name = ":1.42"
        script_utilities._get_private_bus_doc_root = lambda: "/org/a11y/atspi/accessible/1"
        return captured

    def test_empty_role_list_encodes_to_single_zeroed_element(self):
        captured = self._install_capturing_conn()
        result = script_utilities._private_bus_collection_get_matches([])
        self.assertEqual(result, [])

        outer = captured["args"].unpack()
        rule = outer[0]
        # Rule shape: (states, statematch, attrs, attrmatch, roles, rolematch, ifaces, ifacematch, invert)
        _, _, _, _, roles, _, _, _, _ = rule
        self.assertEqual(roles, [0])

    def test_role_values_set_correct_bits(self):
        captured = self._install_capturing_conn()
        # Role 5 → bit 5 of word 0. Role 33 → bit 1 of word 1.
        script_utilities._private_bus_collection_get_matches([5, 33])

        outer = captured["args"].unpack()
        rule = outer[0]
        _, _, _, _, roles, _, _, _, _ = rule
        self.assertEqual(len(roles), 2)
        self.assertEqual(roles[0], 1 << 5)
        self.assertEqual(roles[1], 1 << 1)

    def test_bit_31_is_encoded_as_signed_int32(self):
        # Atspi.Role.LIST == 31 — bit 31 of word 0 is 0x80000000, which
        # overflows signed int32. The bitfield encoder must convert to
        # two's-complement so GLib.Variant("ai", ...) accepts it.
        captured = self._install_capturing_conn()
        script_utilities._private_bus_collection_get_matches([31])

        outer = captured["args"].unpack()
        rule = outer[0]
        _, _, _, _, roles, _, _, _, _ = rule
        self.assertEqual(roles, [-0x80000000])

    def test_with_states_encodes_both_bitfields(self):
        captured = self._install_capturing_conn()
        # Role 8, state 2 and state 40 (requires two words).
        script_utilities._private_bus_collection_get_matches_with_states([8], [2, 40])

        outer = captured["args"].unpack()
        rule = outer[0]
        states, _, _, _, roles, _, _, _, _ = rule
        self.assertEqual(roles[0], 1 << 8)
        # State 2 is bit 2 of word 0; state 40 is bit 8 of word 1.
        self.assertEqual(len(states), 2)
        self.assertEqual(states[0], 1 << 2)
        self.assertEqual(states[1], 1 << 8)


if __name__ == "__main__":
    unittest.main()
