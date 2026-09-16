{
  description = "xmm7360-pci kernel driver and NixOS module for the Fibocom L850-GL WWAN modem.";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    let
      xmm7360-pci = pkgs: kernel: pkgs.callPackage ./pkgs/xmm7360-pci { inherit kernel; };

      xmm7360Module = { ... }: { imports = [ ./modules/xmm7360 ]; };

      xmm7360Overlay = final: prev: {
        xmm7360-pci = xmm7360-pci prev prev.linux;
      };
    in flake-utils.lib.eachSystem [ "x86_64-linux" ] (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };
        xmm7360 = xmm7360-pci pkgs pkgs.linux;
      in {
        packages = {
          inherit xmm7360;
          default = xmm7360;
        };
        checks = { inherit xmm7360; };
      }) // {
        overlays.default = xmm7360Overlay;
        nixosModules.default = xmm7360Module;

        # Backwards-compatible aliases.
        overlay = xmm7360Overlay;
        nixosModule = xmm7360Module;
      };
}
