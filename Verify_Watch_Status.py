#!/usr/bin/env python3
"""Read-only BLE discovery for the J-Style V6G watch.

This phase selects the ASUS BT600 by MAC address, scans only on that BlueZ
controller, verifies the watch, and logs GATT services, characteristics, and
descriptors. It does not enable notifications and it does not write to the
watch.

The previous Verify_Watch_Status.py was not present in this repository when
this discovery phase was implemented, so there was no existing file to back
up. This module does not modify Set_Watch_APN_Only.py.
"""

from __future__ import annotations

import asyncio
import inspect
import logging
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Optional, Sequence

logger = logging.getLogger("v6g.discovery")

ASUS_ADDRESS = "B0:82:E2:25:10:98"
REALTEK_ADDRESS = "00:E0:4C:9D:75:48"
WATCH_ADDRESS = "E7:01:B1:27:B7:92"
WATCH_NAME = "840D9159"
WATCH_ADDRESS_TYPE = "random"
REFUSED_DEVICE_ADDRESS = "78:42:1C:71:E1:36"

ADAPTER_INTERFACE = "org.bluez.Adapter1"
HCI_NAME_RE = re.compile(r"hci\d+")
SCAN_SECONDS = 10.0
CONNECT_TIMEOUT_SECONDS = 20.0

EXIT_OK = 0
EXIT_UNEXPECTED = 1
EXIT_ADAPTER = 2
EXIT_WATCH = 3
EXIT_GATT = 4

OBSERVED_UUID_NOTES = {
    "0000fff0-0000-1000-8000-00805f9b34fb": "matches previously observed FFF0 service",
    "0000fff1-0000-1000-8000-00805f9b34fb": "matches previously observed FFF1 characteristic",
    "0000fff2-0000-1000-8000-00805f9b34fb": "matches previously observed FFF2 characteristic",
    "0000fe59-0000-1000-8000-00805f9b34fb": "Nordic DFU service present; left untouched",
}


class DiscoveryError(Exception):
    """A discovery failure that must stop the run without a fallback radio."""

    def __init__(self, message: str, exit_code: int) -> None:
        super().__init__(message)
        self.exit_code = exit_code


@dataclass(frozen=True)
class ControllerRecord:
    path: str
    hci_name: str
    address: str
    powered: bool
    alias: str
    roles: tuple[str, ...]


@dataclass(frozen=True)
class ObservedDevice:
    address: str
    address_type: Optional[str]
    local_name: Optional[str]
    adapter_path: Optional[str]
    device_path: Optional[str]


def normalize_mac(value: str) -> str:
    return value.strip().upper()


def device_path_for(controller_path: str, address: str) -> str:
    suffix = normalize_mac(address).replace(":", "_")
    return f"{controller_path}/dev_{suffix}"


def parse_controllers(managed_objects: Mapping[str, Any]) -> list[ControllerRecord]:
    """Build controller records from a BlueZ GetManagedObjects result."""
    records: list[ControllerRecord] = []
    for path, interfaces in managed_objects.items():
        if not isinstance(interfaces, Mapping):
            continue
        raw = interfaces.get(ADAPTER_INTERFACE)
        if not isinstance(raw, Mapping):
            continue
        address = raw.get("Address")
        if not isinstance(address, str) or not address.strip():
            continue
        hci_name = str(path).rsplit("/", 1)[-1]
        roles_raw = raw.get("Roles")
        if isinstance(roles_raw, Sequence) and not isinstance(roles_raw, (str, bytes)):
            roles = tuple(str(role) for role in roles_raw)
        else:
            roles = ()
        alias = raw.get("Alias")
        records.append(
            ControllerRecord(
                path=str(path),
                hci_name=hci_name,
                address=normalize_mac(address),
                powered=raw.get("Powered") is True,
                alias=alias if isinstance(alias, str) else "",
                roles=roles,
            )
        )
    return records


def select_asus_controller(controllers: Sequence[ControllerRecord]) -> ControllerRecord:
    """Return the single powered ASUS controller. Never substitute another radio."""
    matches = [controller for controller in controllers if controller.address == ASUS_ADDRESS]
    if not matches:
        realtek_present = any(controller.address == REALTEK_ADDRESS for controller in controllers)
        if realtek_present:
            raise DiscoveryError(
                "ASUS adapter "
                f"{ASUS_ADDRESS} is unavailable. Realtek adapter {REALTEK_ADDRESS} "
                "is present and will not be used.",
                EXIT_ADAPTER,
            )
        raise DiscoveryError(
            f"ASUS adapter {ASUS_ADDRESS} is unavailable. No other adapter will be used.",
            EXIT_ADAPTER,
        )
    if len(matches) != 1:
        raise DiscoveryError(
            f"More than one BlueZ controller reports {ASUS_ADDRESS}. Refusing to choose one.",
            EXIT_ADAPTER,
        )

    chosen = matches[0]
    if chosen.address == REALTEK_ADDRESS:
        raise DiscoveryError(
            f"Refusing Realtek adapter {REALTEK_ADDRESS}.",
            EXIT_ADAPTER,
        )
    if not HCI_NAME_RE.fullmatch(chosen.hci_name):
        raise DiscoveryError(
            f"ASUS controller path {chosen.path} does not end in a BlueZ hci name.",
            EXIT_ADAPTER,
        )
    if chosen.path != f"/org/bluez/{chosen.hci_name}":
        raise DiscoveryError(
            f"ASUS controller path {chosen.path} does not match hci name {chosen.hci_name}.",
            EXIT_ADAPTER,
        )
    if "central" not in chosen.roles:
        raise DiscoveryError(
            f"ASUS adapter {chosen.path} does not advertise the BLE central role.",
            EXIT_ADAPTER,
        )
    if not chosen.powered:
        raise DiscoveryError(
            f"ASUS adapter {chosen.path} is present but Powered is false. "
            "Adapter settings will not be changed.",
            EXIT_ADAPTER,
        )
    return chosen


def cross_check_sysfs(
    controller: ControllerRecord,
    sysfs_root: Path = Path("/sys/class/bluetooth"),
) -> None:
    """Confirm the BlueZ path and the sysfs address belong to the same ASUS radio."""
    if not HCI_NAME_RE.fullmatch(controller.hci_name):
        raise DiscoveryError(
            f"Refusing sysfs lookup for unexpected controller name {controller.hci_name!r}.",
            EXIT_ADAPTER,
        )
    address_file = sysfs_root / controller.hci_name / "address"
    if not address_file.is_file():
        raise DiscoveryError(
            f"sysfs address node {address_file} is missing. "
            "The ASUS controller was not verified, so no adapter will be used.",
            EXIT_ADAPTER,
        )
    sysfs_address = normalize_mac(address_file.read_text(encoding="ascii"))
    if sysfs_address == REALTEK_ADDRESS:
        raise DiscoveryError(
            f"sysfs address for {controller.hci_name} is the Realtek adapter {REALTEK_ADDRESS}.",
            EXIT_ADAPTER,
        )
    if sysfs_address != controller.address or sysfs_address != ASUS_ADDRESS:
        raise DiscoveryError(
            f"D-Bus address {controller.address} does not match sysfs address {sysfs_address} "
            f"for {controller.path}.",
            EXIT_ADAPTER,
        )


def observe_device(device: Any, advertisement: Any) -> ObservedDevice:
    details = device.details if isinstance(getattr(device, "details", None), Mapping) else {}
    props = details.get("props") if isinstance(details.get("props"), Mapping) else {}
    device_path = details.get("path")
    address_type = props.get("AddressType")
    adapter_path = props.get("Adapter")
    local_name = getattr(advertisement, "local_name", None)
    return ObservedDevice(
        address=normalize_mac(str(getattr(device, "address", ""))),
        address_type=address_type if isinstance(address_type, str) else None,
        local_name=local_name if isinstance(local_name, str) else None,
        adapter_path=adapter_path if isinstance(adapter_path, str) else None,
        device_path=device_path if isinstance(device_path, str) else None,
    )


def select_watch(
    observations: Sequence[ObservedDevice],
    controller_path: str,
) -> ObservedDevice:
    """Select the watch only when address, address type, name, and adapter all match."""
    expected_path = device_path_for(controller_path, WATCH_ADDRESS)
    other_name_hits = [
        observed
        for observed in observations
        if observed.local_name == WATCH_NAME and observed.address != WATCH_ADDRESS
    ]
    if other_name_hits:
        addresses = ", ".join(observed.address for observed in other_name_hits)
        raise DiscoveryError(
            f"Advertised name {WATCH_NAME} was seen on {addresses}. "
            "Refusing to connect to a different address.",
            EXIT_WATCH,
        )

    candidates = [observed for observed in observations if observed.address == WATCH_ADDRESS]
    if not candidates:
        raise DiscoveryError(
            f"J-Style watch {WATCH_ADDRESS} was not discovered on {controller_path}.",
            EXIT_WATCH,
        )
    if len(candidates) != 1:
        raise DiscoveryError(
            f"Watch address {WATCH_ADDRESS} was observed more than once. Refusing to choose an object.",
            EXIT_WATCH,
        )

    chosen = candidates[0]
    if chosen.address == REFUSED_DEVICE_ADDRESS:
        raise DiscoveryError(
            f"Refusing Nespresso Vertuo address {REFUSED_DEVICE_ADDRESS}.",
            EXIT_WATCH,
        )
    if chosen.address_type != WATCH_ADDRESS_TYPE:
        raise DiscoveryError(
            f"Watch address {WATCH_ADDRESS} has address type {chosen.address_type!r}, "
            f"expected {WATCH_ADDRESS_TYPE!r}.",
            EXIT_WATCH,
        )
    if chosen.local_name != WATCH_NAME:
        raise DiscoveryError(
            f"Watch address {WATCH_ADDRESS} has local name {chosen.local_name!r}, "
            f"expected {WATCH_NAME!r}.",
            EXIT_WATCH,
        )
    if chosen.adapter_path != controller_path:
        raise DiscoveryError(
            f"Watch object adapter {chosen.adapter_path} is not the ASUS controller {controller_path}.",
            EXIT_WATCH,
        )
    if chosen.device_path != expected_path:
        raise DiscoveryError(
            f"Watch object path {chosen.device_path} is not {expected_path}.",
            EXIT_WATCH,
        )
    return chosen


def bytes_as_hex(value: bytes | bytearray) -> str:
    return bytes(value).hex()


def format_advertisement(device: Any, advertisement: Any) -> str:
    """Format one advertisement as raw hex fields without interpreting a protocol."""
    observed = observe_device(device, advertisement)
    details = device.details if isinstance(getattr(device, "details", None), Mapping) else {}
    props = details.get("props") if isinstance(details.get("props"), Mapping) else {}
    fields = [
        f"address={observed.address}",
        f"address_type={observed.address_type}",
        f"local_name={observed.local_name}",
        f"alias={props.get('Alias')}",
        f"rssi={getattr(advertisement, 'rssi', None)}",
        f"tx_power={getattr(advertisement, 'tx_power', None)}",
        f"adapter={observed.adapter_path}",
        f"device_path={observed.device_path}",
        f"service_uuids={list(getattr(advertisement, 'service_uuids', None) or [])}",
    ]
    manufacturer = getattr(advertisement, "manufacturer_data", None) or {}
    if isinstance(manufacturer, Mapping):
        for company_id in sorted(manufacturer, key=lambda item: str(item)):
            payload = manufacturer[company_id]
            if isinstance(payload, (bytes, bytearray)):
                fields.append(f"manufacturer[{company_id}]={bytes_as_hex(payload)}")
    service_data = getattr(advertisement, "service_data", None) or {}
    if isinstance(service_data, Mapping):
        for service_uuid in sorted(service_data, key=str):
            payload = service_data[service_uuid]
            if isinstance(payload, (bytes, bytearray)):
                fields.append(f"service_data[{service_uuid}]={bytes_as_hex(payload)}")
    advertising_data = props.get("AdvertisingData")
    if isinstance(advertising_data, Mapping):
        for ad_type in sorted(advertising_data, key=lambda item: str(item)):
            payload = advertising_data[ad_type]
            if isinstance(payload, (bytes, bytearray)):
                fields.append(f"ad_type[{ad_type}]={bytes_as_hex(payload)}")
    advertising_flags = props.get("AdvertisingFlags")
    if isinstance(advertising_flags, (bytes, bytearray)):
        fields.append(f"advertising_flags={bytes_as_hex(advertising_flags)}")
    return " ".join(fields)


def bound_watch_device(device: Any, controller_path: str) -> bool:
    """Re-check the object that would be passed to BleakClient."""
    details = device.details if isinstance(getattr(device, "details", None), Mapping) else {}
    props = details.get("props") if isinstance(details.get("props"), Mapping) else {}
    return (
        normalize_mac(str(getattr(device, "address", ""))) == WATCH_ADDRESS
        and props.get("AddressType") == WATCH_ADDRESS_TYPE
        and props.get("Adapter") == controller_path
        and details.get("path") == device_path_for(controller_path, WATCH_ADDRESS)
    )


def uuid_note(uuid: str) -> str:
    return OBSERVED_UUID_NOTES.get(uuid.lower(), "")


def descriptor_flags(descriptor: Any) -> list[str]:
    obj = getattr(descriptor, "obj", None)
    if isinstance(obj, tuple) and len(obj) == 2 and isinstance(obj[1], Mapping):
        flags = obj[1].get("Flags")
        if isinstance(flags, Sequence) and not isinstance(flags, (str, bytes)):
            return [str(flag) for flag in flags]
    return []


def _backend_args(hci_name: str, signature: inspect.Signature) -> dict[str, Any]:
    if not HCI_NAME_RE.fullmatch(hci_name):
        raise DiscoveryError(
            "Refusing to open a controller whose name was not resolved from the ASUS MAC.",
            EXIT_ADAPTER,
        )
    if "bluez" in signature.parameters:
        args = {"bluez": {"adapter": hci_name}}
    elif "adapter" in signature.parameters:
        args = {"adapter": hci_name}
    else:
        raise DiscoveryError(
            "Installed Bleak cannot pin a controller. Refusing to use the default adapter.",
            EXIT_ADAPTER,
        )
    bluez_args = args.get("bluez")
    pinned = bluez_args.get("adapter") if isinstance(bluez_args, Mapping) else args.get("adapter")
    if pinned != hci_name:
        raise DiscoveryError(
            "Controller binding was not explicit. Refusing to continue.",
            EXIT_ADAPTER,
        )
    return args


def scanner_args(hci_name: str, scanner_type: type) -> dict[str, Any]:
    args = _backend_args(hci_name, inspect.signature(scanner_type.__init__))
    bluez_args = args.get("bluez")
    if isinstance(bluez_args, dict):
        # Host-side scan filter only. This does not change watch configuration.
        bluez_args["filters"] = {"DuplicateData": True}
    return args


def client_args(hci_name: str, client_type: type) -> dict[str, Any]:
    signature = inspect.signature(client_type.__init__)
    args = _backend_args(hci_name, signature)
    if "pair" in signature.parameters:
        args["pair"] = False
    if "timeout" in signature.parameters:
        args["timeout"] = CONNECT_TIMEOUT_SECONDS
    return args


def _import_bleak() -> tuple[Any, Any, tuple[type[BaseException], ...]]:
    try:
        from bleak import BleakClient, BleakScanner
        from bleak.exc import BleakDBusError, BleakError, BleakGATTProtocolError
    except ImportError as exc:
        raise DiscoveryError(
            f"Bleak is not installed: {exc}",
            EXIT_ADAPTER,
        ) from exc
    return BleakScanner, BleakClient, (BleakGATTProtocolError, BleakDBusError, BleakError, TimeoutError)


async def fetch_managed_objects() -> dict[str, Any]:
    try:
        from dbus_fast import BusType, Message, MessageType, unpack_variants
        from dbus_fast.aio.message_bus import MessageBus
    except ImportError as exc:
        raise DiscoveryError(
            f"dbus-fast is not installed: {exc}",
            EXIT_ADAPTER,
        ) from exc

    bus = MessageBus(bus_type=BusType.SYSTEM)
    try:
        try:
            await bus.connect()
        except Exception as exc:
            raise DiscoveryError(
                f"Could not connect to the system D-Bus for BlueZ: {exc}",
                EXIT_ADAPTER,
            ) from exc
        reply = await bus.call(
            Message(
                destination="org.bluez",
                path="/",
                interface="org.freedesktop.DBus.ObjectManager",
                member="GetManagedObjects",
            )
        )
    finally:
        bus.disconnect()

    if reply is None or reply.message_type != MessageType.METHOD_RETURN or not reply.body:
        raise DiscoveryError(
            "BlueZ GetManagedObjects failed. The ASUS adapter was not verified.",
            EXIT_ADAPTER,
        )
    managed = unpack_variants(reply.body[0])
    if not isinstance(managed, dict):
        raise DiscoveryError(
            "BlueZ GetManagedObjects returned an unexpected payload.",
            EXIT_ADAPTER,
        )
    return managed


async def resolve_asus_controller() -> ControllerRecord:
    managed = await fetch_managed_objects()
    controllers = parse_controllers(managed)
    if not controllers:
        raise DiscoveryError(
            "BlueZ reported no local adapters. Refusing to scan.",
            EXIT_ADAPTER,
        )
    for controller in controllers:
        logger.info(
            "controller path=%s hci=%s address=%s powered=%s roles=%s alias=%s",
            controller.path,
            controller.hci_name,
            controller.address,
            controller.powered,
            ",".join(controller.roles) or "-",
            controller.alias or "-",
        )
        if controller.address == REALTEK_ADDRESS:
            logger.warning(
                "Refusing Realtek adapter address=%s path=%s",
                controller.address,
                controller.path,
            )
    chosen = select_asus_controller(controllers)
    cross_check_sysfs(chosen)
    logger.info(
        "Resolved ASUS adapter path=%s hci=%s address=%s",
        chosen.path,
        chosen.hci_name,
        chosen.address,
    )
    return chosen


def log_advertisement(device: Any, advertisement: Any) -> None:
    observed = observe_device(device, advertisement)
    if observed.address == REFUSED_DEVICE_ADDRESS:
        logger.warning(
            "Refusing Nespresso Vertuo address=%s path=%s",
            observed.address,
            observed.device_path,
        )
    logger.info("ADV %s", format_advertisement(device, advertisement))


async def discover_watch(controller: ControllerRecord) -> Any:
    bleak_scanner, _, error_types = _import_bleak()
    kwargs = scanner_args(controller.hci_name, bleak_scanner)
    logger.info(
        "Scanning %.1f s on path=%s hci=%s address=%s",
        SCAN_SECONDS,
        controller.path,
        controller.hci_name,
        controller.address,
    )
    try:
        discovered = await bleak_scanner.discover(
            timeout=SCAN_SECONDS,
            return_adv=True,
            detection_callback=log_advertisement,
            scanning_mode="active",
            **kwargs,
        )
    except error_types as exc:
        logger.error("Scan on ASUS adapter %s failed: %s", controller.path, exc)
        raise DiscoveryError(str(exc), EXIT_ADAPTER) from exc
    if not isinstance(discovered, dict):
        raise DiscoveryError(
            "Scanner returned no advertisement map. Refusing to connect.",
            EXIT_WATCH,
        )
    observations: list[ObservedDevice] = []
    devices_by_identity: list[tuple[ObservedDevice, Any]] = []
    for device, advertisement in discovered.values():
        observed = observe_device(device, advertisement)
        observations.append(observed)
        devices_by_identity.append((observed, device))
        if observed.address == REFUSED_DEVICE_ADDRESS:
            logger.warning(
                "Final snapshot still contains refused Nespresso address=%s",
                observed.address,
            )
    chosen = select_watch(observations, controller.path)
    for observed, device in devices_by_identity:
        if observed == chosen:
            logger.info(
                "Verified watch address=%s address_type=%s name=%s adapter=%s path=%s",
                chosen.address,
                chosen.address_type,
                chosen.local_name,
                chosen.adapter_path,
                chosen.device_path,
            )
            return device
    raise DiscoveryError(
        "Watch verification succeeded but the matching BLE object was not retained.",
        EXIT_WATCH,
    )


def log_gatt_tree(services: Any) -> None:
    service_list = sorted(list(services), key=lambda service: (service.handle, service.uuid))
    if not service_list:
        logger.warning("GATT enumeration returned no services.")
    for service in service_list:
        note = uuid_note(service.uuid)
        logger.info(
            "SERVICE uuid=%s handle=%s note=%s",
            service.uuid,
            service.handle,
            note or "-",
        )
        characteristics = sorted(service.characteristics, key=lambda item: (item.handle, item.uuid))
        for characteristic in characteristics:
            properties = ",".join(characteristic.properties)
            logger.info(
                "CHAR uuid=%s handle=%s properties=%s service=%s note=%s",
                characteristic.uuid,
                characteristic.handle,
                properties or "-",
                service.uuid,
                uuid_note(characteristic.uuid) or "-",
            )
            descriptors = sorted(characteristic.descriptors, key=lambda item: (item.handle, item.uuid))
            for descriptor in descriptors:
                flags = ",".join(descriptor_flags(descriptor))
                logger.info(
                    "DESC uuid=%s handle=%s flags=%s char=%s note=%s",
                    descriptor.uuid,
                    descriptor.handle,
                    flags or "-",
                    characteristic.uuid,
                    uuid_note(descriptor.uuid) or "-",
                )


async def enumerate_gatt(device: Any, controller: ControllerRecord) -> None:
    _, bleak_client_type, error_types = _import_bleak()
    protocol_error = error_types[0]
    kwargs = client_args(controller.hci_name, bleak_client_type)

    def on_disconnect(client: Any) -> None:
        logger.warning("Link disconnected address=%s", getattr(client, "address", "unknown"))

    if not bound_watch_device(device, controller.path):
        raise DiscoveryError(
            "Refusing to connect because the watch object is not bound to the resolved ASUS controller.",
            EXIT_WATCH,
        )

    client = bleak_client_type(
        device,
        disconnected_callback=on_disconnect,
        **kwargs,
    )
    try:
        logger.info(
            "Connecting read-only timeout=%.1f s adapter=%s watch=%s",
            CONNECT_TIMEOUT_SECONDS,
            controller.path,
            WATCH_ADDRESS,
        )
        await client.connect()
        if not client.is_connected:
            raise DiscoveryError("Connect returned without an active link.", EXIT_GATT)
        if normalize_mac(str(client.address)) != WATCH_ADDRESS:
            raise DiscoveryError(
                f"Connected address {client.address} is not the verified watch {WATCH_ADDRESS}.",
                EXIT_GATT,
            )
        log_gatt_tree(client.services)
        logger.info(
            "Read-only discovery finished. No notification was enabled and no characteristic was written."
        )
    except protocol_error as exc:
        logger.error("BleakGATTProtocolError during read-only discovery: %s", exc)
        raise DiscoveryError(str(exc), EXIT_GATT) from exc
    except error_types[1:] as exc:
        logger.error("BLE discovery failed: %s", exc)
        raise DiscoveryError(str(exc), EXIT_GATT) from exc
    finally:
        try:
            if getattr(client, "is_connected", False):
                await client.disconnect()
                logger.info("Disconnected from %s", WATCH_ADDRESS)
        except Exception:
            logger.exception("Disconnect failed")


async def run_discovery() -> None:
    logger.info(
        "READ-ONLY V6G discovery. Notifications, CCCD writes, and application writes are disabled."
    )
    logger.info(
        "Expected watch address=%s address_type=%s name=%s",
        WATCH_ADDRESS,
        WATCH_ADDRESS_TYPE,
        WATCH_NAME,
    )
    controller = await resolve_asus_controller()
    device = await discover_watch(controller)
    await enumerate_gatt(device, controller)


def main() -> int:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    try:
        asyncio.run(run_discovery())
    except DiscoveryError as exc:
        logger.error("%s", exc)
        return exc.exit_code
    except Exception:
        logger.exception("Unexpected discovery failure")
        return EXIT_UNEXPECTED
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
