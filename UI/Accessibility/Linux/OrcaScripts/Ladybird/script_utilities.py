"""Ladybird Orca script utilities: bridge Orca to Ladybird’s private ATK bridge.

Orca’s pyatspi is hard-wired to a single AT-SPI2 bus (the session a11y
bus), where Qt’s bridge exposes Ladybird. In parallel, Ladybird runs a
direct ATK bridge on a private D-Bus bus that provides the full set of
AT-SPI2 interfaces (Collection, Hypertext, Document, Cache) that Qt’s
bridge lacks.

This module queries the private bus for operations the Qt bridge does
poorly and maps the results back to Qt-bridge ``Atspi.Accessible``
objects via the shared ``node-id`` attribute, so Orca keeps navigating
the Qt bridge it already sees.

Overrides installed here:

1. ``AXUtilitiesCollection.find_all_with_role[_and_all_states]`` — use
   the private bus’s ``Collection.GetMatches`` when Qt’s bridge returns
   empty (structural navigation: H, K, I, L, …).

2. ``Utilities.active_document`` — fall back to a window DFS when
   Orca’s default EMBEDS-based path fails, because Qt’s bridge does
   not expose the EMBEDS relation.

3. ``SayAllPresenter.say_all`` — move the locus to document content
   before Say All begins, so it reads page content instead of chrome.

4. ``AXEventSynthesizer.scroll_to_center`` — also emit
   ``ScrollSubstringTo`` (which Qt handles) alongside
   ``ScrollSubstringToPoint`` (which Qt does not), so the focus ring
   follows structural navigation.
"""

from __future__ import annotations

import logging

from orca import focus_manager
from orca.ax_object import AXObject
from orca.ax_utilities import AXUtilities
from orca.scripts import web

# Module logger; silent by default. Users or CI can enable with:
#     logging.getLogger("ladybird.orca").setLevel(logging.DEBUG)
_log = logging.getLogger("ladybird.orca")


# --- Private AT-SPI2 bus connection ---

_private_bus_connection = None
_private_bus_name = None
_private_bus_doc_root = None


def _connect_private_bus():
    """Connect to Ladybird’s private AT-SPI2 bus.

    Reads the bus address from the well-known file written by
    PrivateAccessibilityBus on Ladybird startup."""

    global _private_bus_connection, _private_bus_name

    if _private_bus_connection is not None:
        return _private_bus_connection

    import os

    try:
        import gi

        gi.require_version("Gio", "2.0")
        from gi.repository import Gio
        from gi.repository import GLib
    except Exception:
        return None

    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
    address = None

    for entry in os.listdir(runtime_dir):
        if entry.startswith("ladybird-a11y-") and entry.endswith(".address"):
            try:
                with open(os.path.join(runtime_dir, entry)) as f:
                    address = f.read().strip()
                if address:
                    break
            except Exception:
                continue

    if not address:
        _log.debug("No private bus address file found")
        return None

    try:
        conn = Gio.DBusConnection.new_for_address_sync(
            address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None,
            None,
        )
        _private_bus_connection = conn

        # Find the Ladybird ATK bridge by probing each unique name for one whose root has the
        # application role — that’s the bridge (vs. registryd which has a desktop-frame root).
        result = conn.call_sync(
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "ListNames",
            None,
            GLib.VariantType.new("(as)"),
            Gio.DBusCallFlags.NONE,
            1000,
            None,
        )
        names = result.get_child_value(0).unpack()
        for name in names:
            if not name.startswith(":"):
                continue
            try:
                r = conn.call_sync(
                    name,
                    "/org/a11y/atspi/accessible/root",
                    "org.a11y.atspi.Accessible",
                    "GetRoleName",
                    None,
                    GLib.VariantType.new("(s)"),
                    Gio.DBusCallFlags.NONE,
                    500,
                    None,
                )
                if r.get_child_value(0).get_string() == "application":
                    _private_bus_name = name
                    break
            except Exception:
                continue

        _log.info("Connected to private bus at %s, name=%s", address, _private_bus_name)
        return conn
    except Exception as e:
        _log.warning("Failed to connect to private bus: %s", e)
        return None


def _get_private_bus_doc_root():
    """Find the document root path on the private bus.

    The application root is at ``/org/a11y/atspi/accessible/root``; its
    first child is the document root. The document path is
    non-deterministic (depends on object registration order), so we
    discover it dynamically and cache it."""

    global _private_bus_doc_root
    if _private_bus_doc_root is not None:
        return _private_bus_doc_root

    conn = _connect_private_bus()
    if conn is None or _private_bus_name is None:
        return None

    try:
        from gi.repository import Gio
        from gi.repository import GLib

        c = conn.call_sync(
            _private_bus_name,
            "/org/a11y/atspi/accessible/root",
            "org.a11y.atspi.Accessible",
            "GetChildren",
            None,
            GLib.VariantType.new("(a(so))"),
            Gio.DBusCallFlags.NONE,
            2000,
            None,
        )
        children = c.get_child_value(0)
        if children.n_children() > 0:
            _private_bus_doc_root = children.get_child_value(0).get_child_value(1).get_string()
            return _private_bus_doc_root
    except Exception:
        pass
    return None


def _encode_bitfield(values):
    """Pack a list of bit positions into the signed int32 word array expected
    by AT-SPI2 Collection match rules.

    The Collection ``MatchRule`` D-Bus signature is ``(ai...)`` where the first
    ``ai`` is the state array and later on there’s another ``ai`` for the role
    array — both signed int32. When a bit position is 31 (or 63, 95, …),
    ``1 << 31`` is ``0x80000000`` which is out of range for a signed int32, so
    we convert each word to its two’s-complement signed representation."""

    max_val = max(values) if values else 0
    n_ints = (max_val // 32) + 1
    bits = [0] * n_ints
    for v in values:
        bits[v // 32] |= 1 << (v % 32)
    # GLib.Variant("ai", ...) rejects values >= 0x80000000; convert to signed int32.
    return [b - 0x100000000 if b & 0x80000000 else b for b in bits]


def _private_bus_collection_get_matches(atspi_role_values):
    """Call ``Collection.GetMatches`` on the private bus for any of the given AT-SPI2 roles.

    Returns a list of object paths, or an empty list on failure."""

    conn = _connect_private_bus()
    if conn is None or _private_bus_name is None:
        return []

    try:
        from gi.repository import Gio
        from gi.repository import GLib

        role_bits = _encode_bitfield(atspi_role_values)

        MATCH_ALL = 1
        MATCH_ANY = 2
        SORT_ORDER_CANONICAL = 1

        args = GLib.Variant(
            "((aiia{ss}iaiiasib)uib)",
            (
                ([], MATCH_ALL, {}, MATCH_ALL, role_bits, MATCH_ANY, [], MATCH_ALL, False),
                SORT_ORDER_CANONICAL,
                0,  # count=0 means unlimited
                True,  # traverse
            ),
        )

        doc_path = _get_private_bus_doc_root()
        if not doc_path:
            return []
        result = conn.call_sync(
            _private_bus_name,
            doc_path,
            "org.a11y.atspi.Collection",
            "GetMatches",
            args,
            GLib.VariantType.new("(a(so))"),
            Gio.DBusCallFlags.NONE,
            5000,
            None,
        )

        matches = result.get_child_value(0)
        paths = []
        for i in range(matches.n_children()):
            paths.append(matches.get_child_value(i).get_child_value(1).get_string())
        return paths
    except Exception as e:
        _log.debug("Collection.GetMatches failed: %s", e)
        return []


def _private_bus_collection_get_matches_with_states(atspi_role_values, atspi_state_values):
    """``Collection.GetMatches`` filtered by roles AND required states."""

    conn = _connect_private_bus()
    if conn is None or _private_bus_name is None:
        return []

    try:
        from gi.repository import Gio
        from gi.repository import GLib

        role_bits = _encode_bitfield(atspi_role_values)
        state_bits = _encode_bitfield(atspi_state_values)

        MATCH_ALL = 1
        MATCH_ANY = 2
        SORT_ORDER_CANONICAL = 1

        args = GLib.Variant(
            "((aiia{ss}iaiiasib)uib)",
            (
                (state_bits, MATCH_ALL, {}, MATCH_ALL, role_bits, MATCH_ANY, [], MATCH_ALL, False),
                SORT_ORDER_CANONICAL,
                0,
                True,
            ),
        )

        doc_path = _get_private_bus_doc_root()
        if not doc_path:
            return []
        result = conn.call_sync(
            _private_bus_name,
            doc_path,
            "org.a11y.atspi.Collection",
            "GetMatches",
            args,
            GLib.VariantType.new("(a(so))"),
            Gio.DBusCallFlags.NONE,
            5000,
            None,
        )

        matches = result.get_child_value(0)
        paths = []
        for i in range(matches.n_children()):
            paths.append(matches.get_child_value(i).get_child_value(1).get_string())
        return paths
    except Exception as e:
        _log.debug("Collection.GetMatches (with states) failed: %s", e)
        return []


# --- Node-id mapping: private bus ↔ Qt bridge ---

# Cached mapping from node-id to Qt-bridge Atspi.Accessible, rebuilt when the Qt tree root
# changes (indicating a new accessibility tree arrived).
_node_id_cache = {}  # node-id string -> Atspi.Accessible
_node_id_cache_root = None  # Qt root used to build the cache


def _rebuild_node_id_cache(qt_root):
    """Walk the Qt bridge tree and index every node by its ``node-id`` attribute."""

    global _node_id_cache, _node_id_cache_root
    _node_id_cache = {}
    _node_id_cache_root = qt_root

    def walk(obj, depth=0):
        if obj is None or depth > 50:
            return
        try:
            attrs = obj.get_attributes()
            if attrs:
                for key, val in attrs.items():
                    if key == "node-id":
                        _node_id_cache[val] = obj
                        break
        except Exception:
            pass
        for i in range(AXObject.get_child_count(obj)):
            try:
                walk(AXObject.get_child(obj, i), depth + 1)
            except Exception:
                pass

    walk(qt_root)


def _map_private_bus_to_qt_bridge(private_bus_paths, qt_root):
    """Map private-bus object paths to Qt-bridge ``Atspi.Accessible`` objects via the
    shared ``node-id`` attribute.

    Uses the cached node-id index to avoid re-walking the Qt tree on every call."""

    if not private_bus_paths:
        return []

    conn = _connect_private_bus()
    if conn is None or _private_bus_name is None:
        return []

    if qt_root is not _node_id_cache_root:
        _rebuild_node_id_cache(qt_root)

    try:
        from gi.repository import Gio
        from gi.repository import GLib

        results = []
        for path in private_bus_paths:
            try:
                a = conn.call_sync(
                    _private_bus_name,
                    path,
                    "org.a11y.atspi.Accessible",
                    "GetAttributes",
                    None,
                    GLib.VariantType.new("(a{ss})"),
                    Gio.DBusCallFlags.NONE,
                    2000,
                    None,
                )
                attrs = a.get_child_value(0).unpack()
                node_id = attrs.get("node-id")
                if node_id and node_id in _node_id_cache:
                    qt_obj = _node_id_cache[node_id]
                    if not AXObject.is_dead(qt_obj):
                        results.append(qt_obj)
                    else:
                        # Entry is stale — rebuild and retry once.
                        _rebuild_node_id_cache(qt_root)
                        qt_obj = _node_id_cache.get(node_id)
                        if qt_obj and not AXObject.is_dead(qt_obj):
                            results.append(qt_obj)
            except Exception:
                pass
        return results
    except Exception:
        return []


# --- Monkey patches (installed once per process) ---

_collection_fallback_installed = False
_sayall_patched = False
_scroll_patched = False


def _install_collection_fallback_patch():
    """Wrap ``AXUtilitiesCollection.find_all_with_role[_and_all_states]`` so they
    consult the private bus when Qt’s bridge returns empty.

    Forward-compatible: once Qt’s Collection works (6.11+), the private-bus path
    is never taken because Qt returns results first."""

    global _collection_fallback_installed
    if _collection_fallback_installed:
        return
    _collection_fallback_installed = True

    try:
        from orca.ax_utilities_collection import AXUtilitiesCollection

        _original_find_all_with_role = AXUtilitiesCollection.find_all_with_role

        @staticmethod
        def _patched_find_all_with_role(root, role_list, pred=None):
            try:
                results = _original_find_all_with_role(root, role_list, pred)
                if results:
                    return results
            except Exception:
                pass
            paths = _private_bus_collection_get_matches(role_list)
            if paths:
                return _map_private_bus_to_qt_bridge(paths, root)
            return []

        AXUtilitiesCollection.find_all_with_role = _patched_find_all_with_role

        _original_find_all_with_role_and_all_states = AXUtilitiesCollection.find_all_with_role_and_all_states

        @staticmethod
        def _patched_find_all_with_role_and_all_states(root, role_list, state_list, pred=None):
            try:
                results = _original_find_all_with_role_and_all_states(root, role_list, state_list, pred)
                if results:
                    return results
            except Exception:
                pass
            paths = _private_bus_collection_get_matches_with_states(role_list, state_list)
            if paths:
                return _map_private_bus_to_qt_bridge(paths, root)
            return []

        AXUtilitiesCollection.find_all_with_role_and_all_states = _patched_find_all_with_role_and_all_states
    except Exception as e:
        _log.warning("Failed to install Collection fallback patch: %s", e)


def _find_first_text_descendant(obj, depth=0):
    """Return the first descendant of ``obj`` that has a name or non-empty text.

    Used by the Say All patch when ``first_context`` returns the document root
    itself (which has no text of its own): descending to a real text-bearing
    child stops Say All from opening with “Blank”."""

    if obj is None or depth > 15:
        return None
    for i in range(AXObject.get_child_count(obj)):
        child = AXObject.get_child(obj, i)
        if child is None:
            continue
        if AXObject.get_name(child):
            return child
        try:
            ti = child.get_text_iface()
            if ti and ti.get_character_count() > 0:
                return child
        except Exception:
            pass
        found = _find_first_text_descendant(child, depth + 1)
        if found is not None:
            return found
    return None


def _install_sayall_patch():
    """Move the locus into document content before Say All starts.

    Without this, pressing Say All while focus is on chrome (tab bar,
    URL field) causes the screen reader to start reading chrome instead
    of page content."""

    global _sayall_patched
    if _sayall_patched:
        return
    _sayall_patched = True

    try:
        from orca import say_all_presenter

        presenter = say_all_presenter.get_presenter()
        _orig_say_all = presenter.say_all

        def _patched_say_all(script, event=None, notify_user=True, obj=None, offset=None):
            if obj is None and hasattr(script, "utilities"):
                doc = script.utilities.active_document()
                if doc is not None:
                    first_obj, first_offset = script.utilities.first_context(doc, 0)
                    # If ``first_context`` couldn’t descend past the document itself,
                    # find a descendant with actual text content so Say All doesn’t
                    # begin by reading the empty document root (Orca says “Blank”).
                    if first_obj is None or first_obj is doc:
                        descendant = _find_first_text_descendant(doc)
                        if descendant is not None:
                            first_obj, first_offset = descendant, 0
                    if first_obj is not None:
                        script.utilities.set_caret_context(first_obj, first_offset, doc)
                        obj = first_obj
                        offset = first_offset

            return _orig_say_all(script, event, notify_user, obj, offset)

        presenter.say_all = _patched_say_all
        if "sayAllHandler" in presenter._handlers:
            presenter._handlers["sayAllHandler"].function = _patched_say_all
    except Exception as e:
        _log.warning("Failed to install Say All patch: %s", e)


def _install_scroll_patch():
    """Also trigger ``ScrollSubstringTo`` when Orca asks to scroll to center.

    Qt’s bridge implements ``ScrollSubstringTo`` but not
    ``ScrollSubstringToPoint``. Without this patch the focus ring never
    appears during structural navigation because Orca only emits the
    latter."""

    global _scroll_patched
    if _scroll_patched:
        return
    _scroll_patched = True

    try:
        from orca.ax_event_synthesizer import AXEventSynthesizer
        from orca.ax_text import AXText

        _orig_scroll_to_center = AXEventSynthesizer.scroll_to_center

        @staticmethod
        def _patched_scroll_to_center(obj, start_offset=None, end_offset=None):
            _orig_scroll_to_center(obj, start_offset, end_offset)
            try:
                length = AXText.get_character_count(obj)
                if length:
                    if start_offset is None:
                        start_offset = 0
                    if end_offset is None:
                        end_offset = length - 1
                    AXText.scroll_substring_to_location(obj, 0, start_offset, end_offset)
            except Exception:
                pass

        AXEventSynthesizer.scroll_to_center = _patched_scroll_to_center
    except Exception as e:
        _log.warning("Failed to install scroll patch: %s", e)


class Utilities(web.Utilities):
    def __init__(self, script):
        super().__init__(script)
        self._private_bus = _connect_private_bus()
        _install_collection_fallback_patch()
        _install_sayall_patch()
        _install_scroll_patch()

    def clear_cached_objects(self):
        """Clear the private-bus-related caches alongside Orca’s own."""
        global _node_id_cache, _node_id_cache_root, _private_bus_doc_root
        super().clear_cached_objects()
        _node_id_cache = {}
        _node_id_cache_root = None
        _private_bus_doc_root = None

    def active_document(self):
        """Find the active document.

        Orca’s default locates it via the EMBEDS relation on the
        top-level window. Qt’s bridge does not expose EMBEDS, so when
        that fails we DFS the window for a ``document_web`` descendant.
        Multiple documents may exist (one per tab); prefer the one
        whose parent panel is showing."""

        doc = super().active_document()
        if doc is not None:
            return doc

        window = focus_manager.get_manager().get_active_window()
        if window is None:
            return None

        documents = []

        def find_documents(obj, depth=0):
            if obj is None or depth > 15:
                return
            if AXUtilities.is_document_web(obj):
                documents.append(obj)
                return
            for i in range(AXObject.get_child_count(obj)):
                find_documents(AXObject.get_child(obj, i), depth + 1)

        find_documents(window)

        for d in documents:
            parent = AXObject.get_parent(d)
            if parent and AXUtilities.is_showing(parent):
                return d
        return documents[-1] if documents else None
