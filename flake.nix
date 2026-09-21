{
  description = "WS2812B clock — ESP-IDF development shell";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  inputs.systems.url = "github:nix-systems/default";
  inputs.flake-utils = {
    url = "github:numtide/flake-utils";
    inputs.systems.follows = "systems";
  };
  # Deliberately does not follow our nixpkgs: esp-dev pins its own nixpkgs so
  # the ESP-IDF Python environment matches what upstream builds and tests.
  inputs.esp-dev = {
    url = "github:mirrexagon/nixpkgs-esp-dev";
    inputs.flake-utils.follows = "flake-utils";
  };

  outputs =
    {
      nixpkgs,
      flake-utils,
      esp-dev,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        espPkgs = esp-dev.packages.${system};
      in
      {
        devShells.default = pkgs.mkShell {
          # esp-idf-full: ESP-IDF plus every toolchain for all targets
          # (xtensa + riscv32 GCC, esp-clang, ULP, OpenOCD, GDB, ROM ELFs),
          # idf.py, esptool and the rest of the IDF Python environment.
          packages = [
            espPkgs.esp-idf-full
            pkgs.fish
          ];
        };
      }
    );
}
