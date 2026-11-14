"""Windows tray, per-user startup and LAN advertisement adapters."""
from __future__ import annotations

import contextlib
import ipaddress
import os
import socket
import sys
import threading

from .camera import application_icon


def lan_ipv4_addresses() -> list[str]:
    import ifaddr
    addresses = set()
    for adapter in ifaddr.get_adapters():
        for item in adapter.ips:
            if isinstance(item.ip, str):
                ip = ipaddress.ip_address(item.ip)
                if ip.version == 4 and not ip.is_loopback and not ip.is_unspecified and not ip.is_link_local:
                    addresses.add(str(ip))
    return sorted(addresses)


class Advertisement:
    def __init__(self):
        self.zeroconf = self.service = None

    def close(self):
        if self.zeroconf:
            if self.service:
                with contextlib.suppress(Exception):
                    self.zeroconf.unregister_service(self.service)
            self.zeroconf.close()
        self.zeroconf = self.service = None

    def update(self, session, port):
        self.close()
        session.lan_addresses = [f"http://{ip}:{port}" for ip in lan_ipv4_addresses()] if session.config["lan_enabled"] else []
        if not session.config["lan_enabled"] or session.faults["offline"] or not session.lan_addresses:
            return
        from zeroconf import IPVersion, ServiceInfo, Zeroconf
        addresses = [socket.inet_aton(address.split("//", 1)[1].rsplit(":", 1)[0]) for address in session.lan_addresses]
        self.zeroconf = Zeroconf(ip_version=IPVersion.V4Only)
        self.service = ServiceInfo("_moonraker._tcp.local.", f"HF-Virtual-{session.instance_uuid[:12]}._moonraker._tcp.local.",
                                   addresses=addresses, port=port,
                                   properties={"path": "/", "name": session.config["name"], "uuid": session.instance_uuid},
                                   server=f"hf-virtual-{session.instance_uuid[:12]}.local.")
        self.zeroconf.register_service(self.service)


def set_autorun(enabled: bool):
    if os.name != "nt":
        return
    import winreg
    # Persist only a frozen executable entry. Development interpreters must never become login tasks.
    if enabled and not getattr(sys, "frozen", False):
        raise RuntimeError("开机启动仅支持安装包／便携版，请使用发行版启用")
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Run") as key:
        if enabled:
            winreg.SetValueEx(key, "HonorForgeVirtualPrinter", 0, winreg.REG_SZ, f'"{sys.executable}" --no-browser')
        else:
            with contextlib.suppress(FileNotFoundError):
                winreg.DeleteValue(key, "HonorForgeVirtualPrinter")


def tray_icon(open_console, stop):
    import pystray
    return pystray.Icon("HonorForgeVirtualPrinter", application_icon(), "Honor Forge 虚拟打印机",
                        menu=pystray.Menu(pystray.MenuItem("打开中控", lambda *_: open_console(), default=True),
                                          pystray.MenuItem("退出虚拟打印机", lambda icon, _: (stop(), icon.stop()))))
