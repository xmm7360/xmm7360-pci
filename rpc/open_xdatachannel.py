#!/usr/bin/env python3

import configargparse
from pyroute2 import IPRoute
from os.path import join, abspath, dirname
import uuid
import socket
import struct
import dbus
import sys
import time

import rpc
import logging
# must do this before importing pyroute2
logging.basicConfig(level=logging.DEBUG)


parser = configargparse.ArgumentParser(
    description='Hacky tool to bring up XMM7x60 modem',
    default_config_files=[
        '/etc/xmm7360',
        join(dirname(abspath(__file__)), '..', 'xmm7360.ini')
    ],
)

parser.add_argument('-c', '--conf', is_config_file=True)
parser.add_argument('-a', '--apn', required=True, help="Network provider APN")
parser.add_argument('-n', '--nodefaultroute', action="store_true",
                    help="Don't install modem as default route for IP traffic")
parser.add_argument('-m', '--metric', type=int, default=1000,
                    help="Metric for default route (higher is lower priority)")
parser.add_argument('-t', '--ip-fetch-timeout', type=int, default=1,
                    help="Retry interval in seconds when getting IP config")
parser.add_argument('-r', '--noresolv', action="store_true",
                    help="Don't add modem-provided DNS servers to /etc/resolv.conf")
parser.add_argument('-d', '--dbus', action="store_true",
                    help="Activate Networkmanager Connection via DBUS")

cfg, unknown = parser.parse_known_args()

r = None
try:
    r = rpc.XMMRPC()
except Exception as ex:
    logging.error(ex)
    exit()

ipr = IPRoute()

r.execute('UtaMsSmsInit')
r.execute('UtaMsCbsInit')
r.execute('UtaMsNetOpen')
r.execute('UtaMsCallCsInit')
r.execute('UtaMsCallPsInitialize')
r.execute('UtaMsSsInit')
r.execute('UtaMsSimOpenReq')

rpc.do_fcc_unlock(r)
# disable aeroplane mode if had been FCC-locked. first and second args are probably don't-cares
rpc.UtaModeSet(r, 1)

r.execute('UtaMsCallPsAttachApnConfigReq',
          rpc.pack_UtaMsCallPsAttachApnConfigReq(cfg.apn), is_async=True)

attach = r.execute('UtaMsNetAttachReq',
                   rpc.pack_UtaMsNetAttachReq(), is_async=True)
_, status = rpc.unpack('nn', attach['body'])

if status == 0xffffffff:
    logging.info("Attach failed - waiting to see if we just weren't ready")

    while not r.attach_allowed:
        r.pump()

    attach = r.execute('UtaMsNetAttachReq',
                       rpc.pack_UtaMsNetAttachReq(), is_async=True)
    _, status = rpc.unpack('nn', attach['body'])

    if status == 0xffffffff:
        logging.error("Attach failed again, giving up")
        sys.exit(1)

while True:
    ip_addr, dns_values = rpc.get_ip(r)
    if ip_addr is not None:
        break
    interval = cfg.ip_fetch_timeout
    logging.info(f"IP address couldn't be fetched, waiting {interval} seconds")
    time.sleep(interval)

logging.info("IP address: " + str(ip_addr))
logging.info("DNS server(s): " + ', '.join(map(str, dns_values['v4'] + dns_values['v6'])))

idx = ipr.link_lookup(ifname='wwan0')[0]

ipr.flush_addr(index=idx)
ipr.link('set',
         index=idx,
         state='up')
ipr.addr('add',
         index=idx,
         address=ip_addr)


if not cfg.nodefaultroute:
    ipr.route('add',
              dst='default',
              priority=cfg.metric,
              oif=idx)

# Add DNS values to /etc/resolv.conf
if not cfg.noresolv:
    with open('/etc/resolv.conf', 'a') as resolv:
        resolv.write('\n# Added by xmm7360\n')
        for dns in dns_values['v4'] + dns_values['v6']:
            resolv.write('nameserver %s\n' % dns)

# this gives us way too much stuff, which we need
pscr = r.execute('UtaMsCallPsConnectReq',
                 rpc.pack_UtaMsCallPsConnectReq(), is_async=True)
# this gives us a handle we need
dcr = r.execute('UtaRPCPsConnectToDatachannelReq',
                rpc.pack_UtaRPCPsConnectToDatachannelReq())

csr_req = pscr['body'][:-6] + dcr['body'] + b'\x02\x04\0\0\0\0'

r.execute('UtaRPCPSConnectSetupReq', csr_req)

if not cfg.dbus:
    sys.exit(1)

myconnection = None
system_bus = dbus.SystemBus()
service_name = "org.freedesktop.NetworkManager"
proxy = system_bus.get_object(
    service_name, "/org/freedesktop/NetworkManager/Settings")
dproxy = system_bus.get_object(service_name, "/org/freedesktop/NetworkManager")
settings = dbus.Interface(proxy, "org.freedesktop.NetworkManager.Settings")
manager = dbus.Interface(dproxy, "org.freedesktop.NetworkManager")


def dottedQuadToNum(ip):
    return struct.unpack('<L', socket.inet_aton(str(ip)))[0]


def get_connections():
    global myconnection, connection_path
    connection_paths = settings.ListConnections()
    for path in connection_paths:
        con_proxy = system_bus.get_object(service_name, path)
        settings_connection = dbus.Interface(
            con_proxy, "org.freedesktop.NetworkManager.Settings.Connection")
        config = settings_connection.GetSettings()
        s_con = config["connection"]
        print("name:%s uuid:%s type:%s" %
              (s_con["id"], s_con["uuid"], s_con["type"]))
        if s_con["id"] == 'xmm7360':
            myconnection = s_con["uuid"]
            connection_path = path


get_connections()

if (myconnection is not None):
    print("setup %s" % myconnection)
    addr = dbus.Dictionary({"address": ip_addr, "prefix": dbus.UInt32(32)})
    connection_paths = settings.ListConnections()
    for path in connection_paths:
        con_proxy = system_bus.get_object(service_name, path)
        settings_connection = dbus.Interface(
            con_proxy, "org.freedesktop.NetworkManager.Settings.Connection")
        config = settings_connection.GetSettings()
        if config["connection"]["uuid"] != myconnection:
            continue
        print("setup connection")
        connection_path = path
        if "addresses" in config["ipv4"]:
            del config["ipv4"]["addresses"]
        if "address-data" in config["ipv4"]:
            del config["ipv4"]["address-data"]
        if "gateway" in config["ipv4"]:
            del config["ipv4"]["gateway"]
        if "dns" in config["ipv4"]:
            del config["ipv4"]["dns"]

        addr = dbus.Dictionary(
            {"address": ip_addr, "prefix": dbus.UInt32(32)}
        )
        config["ipv4"]["address-data"] = dbus.Array(
            [addr], signature=dbus.Signature("a{sv}")
        )

        dbus_ip = [dottedQuadToNum(ip) for ip in dns_values['v4']]

        config["ipv4"]["gateway"] = ip_addr

        config["ipv4"]["dns"] = dbus.Array([dbus.UInt32(ip) for ip in dbus_ip],
                                           signature=dbus.Signature("u")
                                           )
        settings_connection.Update(config)
else:
    print("adding connection")
    n_con = dbus.Dictionary({"type": "generic", "uuid": str(
        uuid.uuid4()), "id": "xmm7360", "interface-name": "wwan0"})
    addr = dbus.Dictionary(
        {"address": ip_addr, "prefix": dbus.UInt32(32)}
    )

    dbus_ip = [dottedQuadToNum(ip) for ip in dns_values['v4']]
    n_ip4 = dbus.Dictionary(
        {
            "address-data": dbus.Array([addr], signature=dbus.Signature("a{sv}")),
            "gateway": ip_addr,
            "method": "manual",
            "dns": dbus.Array([dbus.UInt32(ip) for ip in dbus_ip], signature=dbus.Signature("u"))
        }
    )
    n_ip6 = dbus.Dictionary({"method": "ignore"})
    add_con = dbus.Dictionary(
        {"connection": n_con, "ipv4": n_ip4, "ipv6": n_ip6})
    settings.AddConnection(add_con)
    get_connections()

devices = manager.GetDevices()

for d in devices:
    dev_proxy = system_bus.get_object("org.freedesktop.NetworkManager", d)
    prop_iface = dbus.Interface(dev_proxy, "org.freedesktop.DBus.Properties")
    props = prop_iface.GetAll("org.freedesktop.NetworkManager.Device")
    if props["Interface"] == "wwan0":
        devpath = d
        print("found Interface: %s" % props["Interface"])
        print("Managed: %s" % props["Managed"])
        if props["Managed"] == 0:
            print("activate")
            prop_iface.Set("org.freedesktop.NetworkManager.Device",
                           "Managed", dbus.Boolean(1))

manager.ActivateConnection(connection_path, devpath, "/")
