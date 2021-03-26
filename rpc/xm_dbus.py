import dbus
import struct
import socket
import uuid

"""
XMM DBUS Library

"""


class DBUS(object):

    def __init__(self):

        self.xmm_connection = None
        self.connection_path = None

        self.system_bus = dbus.SystemBus()
        self.service_name = "org.freedesktop.NetworkManager"

        self.proxy = self.system_bus.get_object(
            self.service_name,
            "/org/freedesktop/NetworkManager/Settings")
        self.dproxy = self.system_bus.get_object(
            self.service_name,
            "/org/freedesktop/NetworkManager")
        self.settings = dbus.Interface(
            self.proxy,
            "org.freedesktop.NetworkManager.Settings")
        self.manager = dbus.Interface(
            self.dproxy,
            "org.freedesktop.NetworkManager")

    def get_connections(self):
        connection_paths = self.settings.ListConnections()

        for path in connection_paths:
            con_proxy = self.system_bus.get_object(self.service_name, path)

            settings_connection = dbus.Interface(
                con_proxy, "org.freedesktop.NetworkManager.Settings.Connection")

            config = settings_connection.GetSettings()
            s_connection = config["connection"]

            if s_connection["id"] == 'xmm7360':
                print("name:%s uuid:%s type:%s" %
                      (s_connection["id"], s_connection["uuid"], s_connection["type"]))

                self.xmm_connection = s_connection
                self.connection_path = path

    def dotted_quad_to_number(self, ip):
        return struct.unpack('<L', socket.inet_aton(str(ip)))[0]

    def dbus_ipv4_dns(self):
        return [dbus.UInt32(struct.unpack('<L', socket.inet_aton(str(ip)))[0]) for ip in self.dns_values['v4']]

    def update_connection(self):
        con_proxy = self.system_bus.get_object(
            self.service_name, self.connection_path)

        settings_connection = dbus.Interface(
            con_proxy, "org.freedesktop.NetworkManager.Settings.Connection")

        config = settings_connection.GetSettings()

        print("update connection")

        if "addresses" in config["ipv4"]:
            del config["ipv4"]["addresses"]

        if "address-data" in config["ipv4"]:
            del config["ipv4"]["address-data"]

        if "gateway" in config["ipv4"]:
            del config["ipv4"]["gateway"]

        if "dns" in config["ipv4"]:
            del config["ipv4"]["dns"]

        addr = dbus.Dictionary({
            "address": self.ip_addr,
            "prefix": dbus.UInt32(32)
        })

        config["ipv4"]["address-data"] = dbus.Array(
            [addr], signature=dbus.Signature("a{sv}")
        )

        config["ipv4"]["gateway"] = self.ip_addr

        config["ipv4"]["dns"] = dbus.Array(
            self.dbus_ipv4_dns(),
            signature=dbus.Signature("u")
        )

        settings_connection.Update(config)

    def add_connection(self):

        n_con = dbus.Dictionary({
            "type": "generic",
            "uuid": str(uuid.uuid4()),
            "id": "xmm7360",
            "interface-name": "wwan0"
        })

        addr = dbus.Dictionary({
            "address": self.ip_addr,
            "prefix": dbus.UInt32(32)
        })

        n_ip4 = dbus.Dictionary({
            "address-data": dbus.Array([addr], signature=dbus.Signature("a{sv}")),
            "gateway": self.ip_addr,
            "method": "manual",
            "dns": dbus.Array(
                self.dbus_ipv4_dns(),
                signature=dbus.Signature("u")
            )
        })

        n_ip6 = dbus.Dictionary({
            "method": "ignore"
        })

        add_con = dbus.Dictionary({
            "connection": n_con,
            "ipv4": n_ip4,
            "ipv6": n_ip6
        })

        self.settings.AddConnection(add_con)

    def setup_network_manager(self, ip_addr, dns_values):
        self.get_connections()

        self.ip_addr = ip_addr
        self.dns_values = dns_values

        # connection found
        if (self.xmm_connection is not None and self.connection_path is not None):
            print("update connection %s" % self.xmm_connection["uuid"])

            self.update_connection()

        else:
            print("adding connection")

            self.add_connection()

            self.get_connections()

        prop_iface = self.get_device_prop_iface()

        if self.device_props["Managed"] == 0:
            print("activate managed interface: %s" %
                  self.device_props["Interface"])

            prop_iface.Set(
                "org.freedesktop.NetworkManager.Device", "Managed", dbus.Boolean(1))

        self.manager.ActivateConnection(
            self.connection_path, self.device_path, "/")

    def get_device_prop_iface(self):
        devices = self.manager.GetDevices()

        for device in devices:

            dev_proxy = self.system_bus.get_object(
                "org.freedesktop.NetworkManager", device)

            prop_iface = dbus.Interface(
                dev_proxy, "org.freedesktop.DBus.Properties")

            props = prop_iface.GetAll("org.freedesktop.NetworkManager.Device")

            if props["Interface"] == "wwan0":
                print("found interface: %s" % props["Interface"])
                print("Managed: %s" % props["Managed"])

                self.device_path = device
                self.device_props = props

                return prop_iface
