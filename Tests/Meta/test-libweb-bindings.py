# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import io
import os
import sys
import unittest

from pathlib import Path

sys.path.insert(0, str(Path(os.environ["LADYBIRD_SOURCE_DIR"]) / "Meta"))

from Generators.libweb_bindings.attributes import write_attribute_getter
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.includes import GeneratedIncludes
from Utils.webidl_parser import parse_module


def generate(attribute: str, interface_name: str = "Example") -> str:
    module = parse_module(
        Path(f"Libraries/LibWeb/HTML/{interface_name}.idl"), f"interface {interface_name} {{ {attribute} }};"
    )
    event_module = parse_module(Path("Libraries/LibWeb/DOM/Event.idl"), "interface Event {};")
    context = GenerationContext([module, event_module])
    interface = context.interfaces[interface_name]
    out = io.StringIO()
    includes = GeneratedIncludes()
    write_attribute_getter(out, context, includes, interface, interface.regular_attributes[0])
    includes.write(out)
    return out.getvalue()


class TestLibWebBindings(unittest.TestCase):
    def test_cross_world_clone_uses_caller_realm(self) -> None:
        output = generate("[CloneAcrossWorlds] readonly attribute any value;")
        self.assertIn("R = world_safe_value(realm, R);", output)
        self.assertIn("throw_dom_exception_if_needed(vm, realm,", output)
        self.assertIn("LibWeb/Bindings/WorldSafeValue.h", output)
        self.assertLess(output.index("implementation.value()"), output.index("R = world_safe_value"))

    def test_cached_cross_world_clone(self) -> None:
        output = generate("[CloneAcrossWorlds=Cached] readonly attribute any value;")
        self.assertIn("R = cached_world_safe_value(realm, R);", output)

    def test_unannotated_any_keeps_receiver_realm(self) -> None:
        output = generate("readonly attribute any value;")
        self.assertNotIn("world_safe_value", output)
        self.assertIn("throw_dom_exception_if_needed(vm, this_object_realm,", output)

    def test_window_event_is_filtered_in_caller_realm(self) -> None:
        attribute = "[Replaceable, FilterErrorEventByWorld] readonly attribute (Event or undefined) event;"
        output = generate(attribute, "Window")
        self.assertIn("R = world_safe_event(realm, R);", output)
        self.assertIn("implementation.event()", output)
        self.assertIn("wrap(host_defined_wrapper_world(realm), realm, object)", output)
        self.assertNotIn("world_safe_event", generate("[Replaceable] readonly attribute (Event or undefined) event;", "Window"))

    def test_rejects_non_any_clone(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "must have type any"):
            generate("[CloneAcrossWorlds] readonly attribute DOMString value;")

    def test_rejects_unknown_clone_mode(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "must be empty or Cached"):
            generate("[CloneAcrossWorlds=Unknown] readonly attribute any value;")


if __name__ == "__main__":
    unittest.main()
