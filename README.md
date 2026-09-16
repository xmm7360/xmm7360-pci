# xmm7360-pci

Kernel driver for the Fibocom L850-GL / Intel XMM7360 LTE modem (PCI ID
`8086:7360`), together with a Nix flake and NixOS module.

This repository is a fork of [xmm7360/xmm7360-pci](https://github.com/xmm7360/xmm7360-pci)
with the Nix packaging merged in. The kernel module is built directly from the
source in this tree.

Please see [DEVICES.md](DEVICES.md) for a list of hardware this has been tested
on, and [INSTALLING.md](INSTALLING.md) for the manual (non-Nix) setup.

## NixOS

Add this repository as a flake input and import the module:

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    xmm7360.url = "github:xmm7360/xmm7360-pci";
  };

  outputs = { nixpkgs, xmm7360, ... }: {
    nixosConfigurations.example = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        xmm7360.nixosModules.default
        {
          hardware.xmm7360 = {
            enable = true;
            autoStart = true;
            config = {
              apn = "3gnet";        # required
              nodefaultroute = false;
              noresolv = true;
            };
          };
        }
      ];
    };
  };
}
```

By default the kernel module is built automatically against your configured
`boot.kernelPackages`. To use a specific package instead, set
`hardware.xmm7360.package`.

When enabled, the `iosm` in-tree driver is blacklisted (it claims the same PCI
device), and the service is automatically restarted after suspend/resume to
reconfigure the modem, which has no power management support.

### Flake outputs

- `packages.x86_64-linux.xmm7360` (also `default`): the driver, built against
  nixpkgs' default `linux` kernel.
- `overlays.default`: adds `xmm7360-pci` to nixpkgs.
- `nixosModules.default`: the NixOS module described above.

### Options

- `hardware.xmm7360.enable` — enable the driver and configuration service.
- `hardware.xmm7360.autoStart` — start the service at boot (default `false`).
- `hardware.xmm7360.config` — flat attribute set written to `xmm7360.ini`.
  Supported keys are the `open_xdatachannel.py` arguments: `apn` (required),
  `nodefaultroute`, `metric`, `ip-fetch-timeout`, `noresolv`, `dbus`.
- `hardware.xmm7360.package` — override the kernel module package (defaults to
  a package built for the running kernel).

If the network refuses packet data — for example because the SIM has no data
allowance, the APN is wrong, or no IP is assigned — the service fails with a
clear error and does not reset/reload the modem for it.

## Manual installation

```
sudo pip install --user pyroute2 ConfigArgParse dbus-python
make && make load
```

If your SIM has a PIN enabled, run:

```
echo 'AT+CPIN="0000"' | sudo tee -a /dev/ttyXMM1
```

Replace `0000` with your PIN. Then bring up the data channel:

```
sudo python3 rpc/open_xdatachannel.py --apn your.apn.here
```

or create `xmm7360.ini` from `xmm7360.ini.sample` and edit at least the APN.
You should receive a `wwan0` interface with an IP and a default route.

### Dependencies

- build-essential
- python3-pyroute2
- python3-configargparse
- python3-dbus

## Status

This release supports native IP.

Power management support is absent. The modem, as configured, turns off during
suspend and needs to be reconfigured on resume.

## Next

Involvement from someone involved in modem control projects like ModemManager
would be welcome to shape the kernel interfaces so it's not too horrible to
bring up.

## License

Dual-licensed under `GPL-2.0 OR BSD-3-Clause`, as declared in `xmm7360.c`.
