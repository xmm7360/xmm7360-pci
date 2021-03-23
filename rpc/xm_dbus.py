import dbus
import struct
import socket
import uuid

class DBUS(object):

    def __init__(self):

        self.xmm_connection     = None
        self.connection_path  = None

        self.system_bus       = dbus.SystemBus()
        self.service_name     = "org.freedesktop.NetworkManager"

        self.proxy            = self.system_bus.get_object(
                                    self.service_name,
                                    "/org/freedesktop/NetworkManager/Settings"
                                )
        self.dproxy           = self.system_bus.get_object(
                                    self.service_name,
                                    "/org/freedesktop/NetworkManager"
                                )
        self.settings         = dbus.Interface(
                                    self.proxy,
                                    "org.freedesktop.NetworkManager.Settings"
                                )
        self.manager          = dbus.Interface(
                                    self.dproxy,
                                    "org.freedesktop.NetworkManager"
                                )

    def dottedQuadToNum(self, ip):
        return struct.unpack('<L', socket.inet_aton(str(ip)))[0]

    def get_connections(self):
        connection_paths = self.settings.ListConnections()

        for path in connection_paths:

            con_proxy = self.system_bus.get_object(self.service_name, path)
            settings_connection = dbus.Interface(con_proxy, "org.freedesktop.NetworkManager.Settings.Connection")

            config = settings_connection.GetSettings()
            s_con = config["connection"]

            if s_con["id"] == 'xmm7360':
                print("name:%s uuid:%s type:%s" % (s_con["id"] ,s_con["uuid"], s_con["type"]))
                self.xmm_connection = s_con["uuid"]
                self.connection_path = path


    def setup_network_manager(self, ip_addr, dns_values):
        self.get_connections()

        if (self.xmm_connection is not None):
            print ("update connection %s" % self.xmm_connection)

            addr = dbus.Dictionary({
                "address": ip_addr,
                "prefix": dbus.UInt32(32)
            })

            connection_paths = self.settings.ListConnections()
            for path in connection_paths:
                con_proxy = self.system_bus.get_object(self.service_name, path)
                settings_connection = dbus.Interface(con_proxy, "org.freedesktop.NetworkManager.Settings.Connection")
                config = settings_connection.GetSettings()

                if config["connection"]["uuid"] != self.xmm_connection:
                    continue

                print ("setup connection") 
                self.connection_path = path

                if "addresses" in config["ipv4"]:
                    del config["ipv4"]["addresses"]

                if "address-data" in config["ipv4"]:
                    del config["ipv4"]["address-data"]

                if "gateway" in config["ipv4"]:
                    del config["ipv4"]["gateway"]

                if "dns" in config["ipv4"]:
                    del config["ipv4"]["dns"]

                addr = dbus.Dictionary({
                    "address": ip_addr,
                    "prefix": dbus.UInt32(32)
                })

                config["ipv4"]["address-data"] = dbus.Array(
                    [addr], signature=dbus.Signature("a{sv}")
                )

                dbus_ip  = [self.dottedQuadToNum(ip) for ip in dns_values['v4']]

                config["ipv4"]["gateway"] = ip_addr

                config["ipv4"]["dns"] = dbus.Array(
                    [dbus.UInt32(ip) for ip in dbus_ip],
                    signature=dbus.Signature("u")
                )
                settings_connection.Update(config)

        else:
            print ("adding connection")
            n_con = dbus.Dictionary({
                "type": "generic",
                "uuid": str(uuid.uuid4()),
                "id": "xmm7360",
                "interface-name" : "wwan0"
            })

            addr = dbus.Dictionary({
                "address": ip_addr,
                "prefix": dbus.UInt32(32)
            })

            dbus_ip  = [self.dottedQuadToNum(ip) for ip in dns_values['v4']]

            n_ip4 = dbus.Dictionary({
                "address-data": dbus.Array([addr], signature=dbus.Signature("a{sv}")),
                "gateway": ip_addr,
                "method": "manual",
                "dns": dbus.Array([dbus.UInt32(ip) for ip in dbus_ip],signature=dbus.Signature("u"))
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
            self.get_connections()

        devices = self.manager.GetDevices()
        for device in devices:
            dev_proxy = self.system_bus.get_object("org.freedesktop.NetworkManager", device)
            prop_iface = dbus.Interface(dev_proxy, "org.freedesktop.DBus.Properties")
            props = prop_iface.GetAll("org.freedesktop.NetworkManager.Device")

            if props["Interface"] == "wwan0":
                devpath = device
                print("found interface: %s" % props["Interface"])
                print("Managed: %s" % props["Managed"])
                if props["Managed"] == 0:
                    print ("activate managed interface: %s" % props["Interface"])
                    prop_iface.Set("org.freedesktop.NetworkManager.Device", "Managed", dbus.Boolean(1))

        self.manager.ActivateConnection(self.connection_path, devpath, "/")
