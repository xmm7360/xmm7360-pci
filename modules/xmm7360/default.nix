{ config, pkgs, lib, ... }:

let
  cfg = config.hardware.xmm7360;

  inherit (lib) mkEnableOption mkIf mkOption types;

  xmm7360Package =
    if cfg.package != null then
      cfg.package
    else
      pkgs.callPackage ../../pkgs/xmm7360-pci {
        kernel = config.boot.kernelPackages.kernel;
      };

  xmm7360ConfigFile =
    pkgs.writeText "xmm7360.ini" (lib.generators.toKeyValue { } cfg.config);
in {
  options.hardware.xmm7360 = {
    enable = mkEnableOption "support for the Fibocom L850-GL (Intel XMM7360) WWAN modem";

    autoStart = mkOption {
      type = types.bool;
      default = false;
      description = "Start the modem configuration service on boot.";
    };

    config = mkOption {
      type = with types; attrsOf (oneOf [ bool int str ]);
      default = { };
      example = {
        apn = "3gnet";
        nodefaultroute = false;
        noresolv = true;
      };
      description = ''
        xmm7360.ini configuration written as a flat attribute set. Supported
        keys are the arguments of `open_xdatachannel.py` (e.g. `apn`,
        `nodefaultroute`, `metric`, `ip-fetch-timeout`, `noresolv`, `dbus`).
        `apn` is required.
      '';
    };

    package = mkOption {
      type = types.nullOr types.package;
      default = null;
      description = ''
        Kernel module package of XMM7360-PCI to use. If left as `null`, the
        package is built automatically for the kernel selected by
        `boot.kernelPackages`.
      '';
    };
  };

  config = mkIf cfg.enable {
    assertions = [{
      assertion = cfg.config ? apn;
      message = ''
        xmm7360.config must contain an `apn` attribute, e.g.
        `xmm7360.config.apn = "your.apn.here";`.
      '';
    }];

    boot.extraModulePackages = [ xmm7360Package ];

    # The in-tree `iosm` driver claims the same PCI device (8086:7360) and
    # would prevent the out-of-tree `xmm7360` driver from binding to it.
    boot.blacklistedKernelModules = [ "iosm" ];

    # The driver has no power management support: the modem powers off during
    # suspend and must be reconfigured when the machine resumes.
    powerManagement.resumeCommands = ''
      ${pkgs.systemd}/bin/systemctl --no-block try-restart xmm7360.service
    '';

    systemd.services.xmm7360 = let
      preStartScript = pkgs.writeShellApplication {
        name = "xmm7360-prestart";
        runtimeInputs = [ pkgs.coreutils pkgs.kmod ];
        text = ''
          # Drop any stale instance of the module.
          modprobe -r xmm7360 2>/dev/null || true

          # The XMM7360 firmware frequently ends up in a state where the
          # command ring no longer comes up, and a plain rmmod/modprobe does
          # not recover it.  Issue a PCI reset (these devices support the ACPI
          # reset method) before loading the module again.
          for dev in /sys/bus/pci/devices/*; do
            if [ -r "$dev/vendor" ] && [ -r "$dev/device" ] \
              && [ "$(cat "$dev/vendor")" = "0x8086" ] \
              && [ "$(cat "$dev/device")" = "0x7360" ]; then
              if [ -w "$dev/reset_method" ]; then
                read -r methods < "$dev/reset_method" || methods=""
                case " $methods " in
                  *" acpi "*) echo acpi > "$dev/reset_method" || true ;;
                esac
              fi
              if [ -w "$dev/reset" ]; then
                echo 1 > "$dev/reset" || true
              fi
            fi
          done

          modprobe xmm7360 || true
        '';
      };
      postStopScript = pkgs.writeShellApplication {
        name = "xmm7360-poststop";
        runtimeInputs = [ pkgs.kmod ];
        text = ''
          rmmod xmm7360 || true
        '';
      };
    in {
      wantedBy = lib.optionals cfg.autoStart [ "multi-user.target" ];
      description = "Configuration service for the Fibocom L850-GL modem";
      # Wait a bit so the freshly inserted device is fully up before probing.
      script = ''
        sleep 10
        ${xmm7360Package}/bin/open_xdatachannel.py -c ${xmm7360ConfigFile}
      '';
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
        # After a reset the modem can take a while to register and attach;
        # kill it only once it is clearly not coming back.
        TimeoutStartSec = "3min";
        ExecStartPre = "${preStartScript}/bin/xmm7360-prestart";
        ExecStopPost = "${postStopScript}/bin/xmm7360-poststop";
        Restart = "on-failure";
        RestartSec = "30s";
        # Exit code 2 means "the network never gave us an IP" (usually no
        # data allowance/credit). That is not a firmware hang, so don't reset
        # and reload the modem over and over for it.
        RestartPreventExitStatus = [ 2 ];
      };
      unitConfig = {
        # Don't spin forever resetting/reloading a modem that never comes up.
        StartLimitBurst = 3;
        StartLimitIntervalSec = "15min";
      };
    };
  };
}
