#!/usr/bin/env python3
"""Host checks for the read-only V6G discovery script. No Bluetooth calls."""

from __future__ import annotations

import ast
import unittest
from pathlib import Path

import Verify_Watch_Status as discovery


class AdapterSelectionTests(unittest.TestCase):
    def asus(self, hci_name: str = "hci2", **overrides: object) -> discovery.ControllerRecord:
        record = discovery.ControllerRecord(
            path=f"/org/bluez/{hci_name}",
            hci_name=hci_name,
            address=discovery.ASUS_ADDRESS,
            powered=True,
            alias="ASUS",
            roles=("central", "peripheral"),
        )
        if overrides:
            values = record.__dict__.copy()
            values.update(overrides)
            record = discovery.ControllerRecord(**values)
        return record

    def test_selects_asus_on_whatever_index_bluez_assigned(self) -> None:
        realtek = discovery.ControllerRecord(
            path="/org/bluez/hci0",
            hci_name="hci0",
            address=discovery.REALTEK_ADDRESS,
            powered=True,
            alias="Realtek",
            roles=("central",),
        )
        chosen = discovery.select_asus_controller([realtek, self.asus("hci3")])
        self.assertEqual(chosen.path, "/org/bluez/hci3")
        self.assertEqual(chosen.address, discovery.ASUS_ADDRESS)

    def test_refuses_realtek_when_asus_is_missing(self) -> None:
        realtek = discovery.ControllerRecord(
            path="/org/bluez/hci1",
            hci_name="hci1",
            address=discovery.REALTEK_ADDRESS,
            powered=True,
            alias="Realtek",
            roles=("central",),
        )
        with self.assertRaises(discovery.DiscoveryError) as raised:
            discovery.select_asus_controller([realtek])
        self.assertEqual(raised.exception.exit_code, discovery.EXIT_ADAPTER)
        self.assertIn(discovery.REALTEK_ADDRESS, str(raised.exception))
        self.assertIn("will not be used", str(raised.exception))

    def test_refuses_powered_off_asus_without_substituting_realtek(self) -> None:
        realtek = discovery.ControllerRecord(
            path="/org/bluez/hci0",
            hci_name="hci0",
            address=discovery.REALTEK_ADDRESS,
            powered=True,
            alias="Realtek",
            roles=("central",),
        )
        with self.assertRaises(discovery.DiscoveryError) as raised:
            discovery.select_asus_controller([realtek, self.asus(powered=False)])
        self.assertIn("Powered is false", str(raised.exception))

    def test_refuses_duplicate_asus_controllers(self) -> None:
        with self.assertRaises(discovery.DiscoveryError):
            discovery.select_asus_controller([self.asus("hci2"), self.asus("hci4")])

    def test_refuses_path_that_does_not_match_hci_name(self) -> None:
        with self.assertRaises(discovery.DiscoveryError):
            discovery.select_asus_controller([self.asus(path="/org/bluez/hci9")])

    def test_refuses_adapter_without_central_role(self) -> None:
        with self.assertRaises(discovery.DiscoveryError):
            discovery.select_asus_controller([self.asus(roles=("peripheral",))])

    def test_parse_requires_real_powered_boolean(self) -> None:
        managed = {
            "/org/bluez/hci4": {
                "org.bluez.Adapter1": {
                    "Address": "b0:82:e2:25:10:98",
                    "Powered": "yes",
                    "Alias": "ASUS",
                    "Roles": ["central"],
                }
            }
        }
        parsed = discovery.parse_controllers(managed)
        self.assertEqual(parsed[0].address, discovery.ASUS_ADDRESS)
        self.assertFalse(parsed[0].powered)
        self.assertEqual(parsed[0].hci_name, "hci4")

    def test_sysfs_must_match_dbus_address(self) -> None:
        root = Path(self.id().replace(".", "_"))
        try:
            controller = self.asus("hci6")
            address_dir = root / "hci6"
            address_dir.mkdir(parents=True)
            (address_dir / "address").write_text("b0:82:e2:25:10:98\n", encoding="ascii")
            discovery.cross_check_sysfs(controller, root)

            (address_dir / "address").write_text(discovery.REALTEK_ADDRESS + "\n", encoding="ascii")
            with self.assertRaises(discovery.DiscoveryError) as raised:
                discovery.cross_check_sysfs(controller, root)
            self.assertIn(discovery.REALTEK_ADDRESS, str(raised.exception))
        finally:
            if root.exists():
                for path in sorted(root.rglob("*"), reverse=True):
                    if path.is_file():
                        path.unlink()
                    else:
                        path.rmdir()
                root.rmdir()


class WatchSelectionTests(unittest.TestCase):
    def watch(self, **overrides: object) -> discovery.ObservedDevice:
        controller = "/org/bluez/hci2"
        observed = discovery.ObservedDevice(
            address=discovery.WATCH_ADDRESS,
            address_type=discovery.WATCH_ADDRESS_TYPE,
            local_name=discovery.WATCH_NAME,
            adapter_path=controller,
            device_path=discovery.device_path_for(controller, discovery.WATCH_ADDRESS),
        )
        if overrides:
            values = observed.__dict__.copy()
            values.update(overrides)
            observed = discovery.ObservedDevice(**values)
        return observed

    def test_accepts_verified_watch_on_asus_path(self) -> None:
        chosen = discovery.select_watch([self.watch()], "/org/bluez/hci2")
        self.assertEqual(chosen.address, discovery.WATCH_ADDRESS)
        self.assertEqual(chosen.local_name, discovery.WATCH_NAME)

    def test_refuses_nespresso_address(self) -> None:
        nespresso = discovery.ObservedDevice(
            address=discovery.REFUSED_DEVICE_ADDRESS,
            address_type="public",
            local_name="Vertuo",
            adapter_path="/org/bluez/hci2",
            device_path="/org/bluez/hci2/dev_78_42_1C_71_E1_36",
        )
        with self.assertRaises(discovery.DiscoveryError) as raised:
            discovery.select_watch([nespresso], "/org/bluez/hci2")
        self.assertEqual(raised.exception.exit_code, discovery.EXIT_WATCH)

    def test_refuses_watch_name_on_another_address(self) -> None:
        other = self.watch(address="11:22:33:44:55:66", device_path="/org/bluez/hci2/dev_11_22_33_44_55_66")
        with self.assertRaises(discovery.DiscoveryError) as raised:
            discovery.select_watch([other], "/org/bluez/hci2")
        self.assertIn(discovery.WATCH_NAME, str(raised.exception))

    def test_refuses_public_address_type(self) -> None:
        with self.assertRaises(discovery.DiscoveryError) as raised:
            discovery.select_watch([self.watch(address_type="public")], "/org/bluez/hci2")
        self.assertIn("random", str(raised.exception))

    def test_refuses_wrong_adapter_association(self) -> None:
        observed = self.watch(
            adapter_path="/org/bluez/hci0",
            device_path=discovery.device_path_for("/org/bluez/hci0", discovery.WATCH_ADDRESS),
        )
        with self.assertRaises(discovery.DiscoveryError) as raised:
            discovery.select_watch([observed], "/org/bluez/hci2")
        self.assertIn("/org/bluez/hci2", str(raised.exception))

    def test_adapter_mac_is_not_a_watch_match(self) -> None:
        radio = self.watch(
            address=discovery.ASUS_ADDRESS,
            device_path=discovery.device_path_for("/org/bluez/hci2", discovery.ASUS_ADDRESS),
        )
        with self.assertRaises(discovery.DiscoveryError):
            discovery.select_watch([radio], "/org/bluez/hci2")

    def test_bound_object_check_requires_asus_path(self) -> None:
        class Device:
            def __init__(self, adapter_path: str) -> None:
                self.address = discovery.WATCH_ADDRESS
                self.details = {
                    "path": discovery.device_path_for(adapter_path, discovery.WATCH_ADDRESS),
                    "props": {
                        "AddressType": discovery.WATCH_ADDRESS_TYPE,
                        "Adapter": adapter_path,
                    },
                }

        self.assertTrue(discovery.bound_watch_device(Device("/org/bluez/hci2"), "/org/bluez/hci2"))
        self.assertFalse(discovery.bound_watch_device(Device("/org/bluez/hci0"), "/org/bluez/hci2"))


class AdvertisementAndBackendTests(unittest.TestCase):
    def test_advertisement_log_is_raw_hex(self) -> None:
        class Device:
            address = discovery.WATCH_ADDRESS
            details = {
                "path": "/org/bluez/hci2/dev_E7_01_B1_27_B7_92",
                "props": {
                    "AddressType": "random",
                    "Alias": discovery.WATCH_NAME,
                    "Adapter": "/org/bluez/hci2",
                    "AdvertisingFlags": b"\x06",
                    "AdvertisingData": {0x09: b"840D9159"},
                },
            }

        class Advertisement:
            local_name = discovery.WATCH_NAME
            rssi = -61
            tx_power = None
            service_uuids = ["0000fff0-0000-1000-8000-00805f9b34fb"]
            manufacturer_data = {0x1234: b"\x01\x02"}
            service_data = {}

        text = discovery.format_advertisement(Device(), Advertisement())
        self.assertIn("manufacturer[4660]=0102", text)
        self.assertIn("ad_type[9]=3834304439313539", text)
        self.assertIn("advertising_flags=06", text)
        self.assertNotIn("apn", text.lower())

    def test_backend_args_pin_resolved_name_and_refuse_an_unspecified_backend(self) -> None:
        class Current:
            def __init__(self, bluez: dict | None = None, pair: bool = True, timeout: float = 1.0) -> None:
                del bluez, pair, timeout

        class Legacy:
            def __init__(self, adapter: str | None = None) -> None:
                del adapter

        class Unpinned:
            def __init__(self, address: str) -> None:
                del address

        pinned = discovery.client_args("hci4", Current)
        self.assertEqual(pinned["bluez"]["adapter"], "hci4")
        self.assertFalse(pinned["pair"])
        self.assertEqual(pinned["timeout"], discovery.CONNECT_TIMEOUT_SECONDS)
        self.assertEqual(discovery.scanner_args("hci4", Current)["bluez"]["filters"], {"DuplicateData": True})
        self.assertEqual(discovery.client_args("hci4", Legacy)["adapter"], "hci4")
        with self.assertRaises(discovery.DiscoveryError):
            discovery.client_args("hci4", Unpinned)


class SourceGuardTests(unittest.TestCase):
    def test_source_has_no_write_notify_or_hardcoded_controller_index(self) -> None:
        source = Path("Verify_Watch_Status.py").read_text(encoding="utf-8")
        tree = ast.parse(source)
        forbidden_attrs = {
            "start_notify",
            "stop_notify",
            "write_gatt_char",
            "write_gatt_descriptor",
            "read_gatt_char",
            "read_gatt_descriptor",
            "get_default_adapter",
            "pair",
        }
        used: set[str] = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Attribute) and node.attr in forbidden_attrs:
                used.add(node.attr)
            if isinstance(node, ast.Name) and node.id in forbidden_attrs:
                used.add(node.id)
        self.assertEqual(used, set())
        for literal in ("hci0", "hci1"):
            self.assertNotIn(literal, source)
        self.assertNotIn("Set_Watch_APN_Only.py", source.replace("does not modify Set_Watch_APN_Only.py", ""))


if __name__ == "__main__":
    unittest.main()
