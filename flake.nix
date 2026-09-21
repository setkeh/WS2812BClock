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
        lib = pkgs.lib;
        espPkgs = esp-dev.packages.${system};
        idf = espPkgs.esp-idf-full;

        # Upstream's .tool-env is written without newlines, which corrupts
        # OPENOCD_SCRIPTS; export the tool vars ourselves from passthru.
        toolEnvExports = lib.concatStringsSep "\n" (
          lib.mapAttrsToList (k: v: "export ${k}=${lib.escapeShellArg v}") idf.passthru.toolEnv
        );

        # The VS Code ESP-IDF extension (v2+) only discovers installs through
        # EIM's eim_idf.json, and loads each one's environment by running its
        # activationScript with `-e` and parsing KEY=VALUE lines.
        idfActivate = pkgs.writeShellScript "esp-idf-activate" ''
          ${toolEnvExports}
          export IDF_PATH=${idf}
          export IDF_TOOLS_PATH=${idf}/tools
          export IDF_PYTHON_ENV_PATH=$(readlink ${idf}/python-env)
          export IDF_PYTHON_CHECK_CONSTRAINTS=no
          export GIT_CONFIG_SYSTEM=${idf}/etc/gitconfig
          export PATH=${
            lib.makeBinPath idf.propagatedBuildInputs
          }:${idf}/tools:${idf}/components/espcoredump:${idf}/components/partition_table:${idf}/components/app_update:$PATH
          if [ "''${1:-}" = "-e" ]; then
            for v in ${lib.concatStringsSep " " (builtins.attrNames idf.passthru.toolEnv)} \
              IDF_PATH IDF_TOOLS_PATH IDF_PYTHON_ENV_PATH IDF_PYTHON_CHECK_CONSTRAINTS GIT_CONFIG_SYSTEM PATH; do
              printf '%s=%s\n' "$v" "''${!v}"
            done
          fi
        '';

        # Adds/refreshes this project's entry in ~/.espressif/tools/eim_idf.json,
        # leaving any other entries (real EIM installs, other projects) intact.
        registerIdf = pkgs.writers.writePython3 "register-esp-idf" { flakeIgnore = [ "E501" ]; } ''
          import json
          import os
          import sys

          project, activate = sys.argv[1], sys.argv[2]
          path = os.path.expanduser("~/.espressif/tools/eim_idf.json")
          os.makedirs(os.path.dirname(path), exist_ok=True)
          try:
              with open(path) as f:
                  data = json.load(f)
          except (FileNotFoundError, json.JSONDecodeError):
              data = {}
          entry_id = "nix-" + os.path.basename(project)
          entry = {
              "id": entry_id,
              "name": "ESP-IDF ${idf.version} (nix: " + os.path.basename(project) + ")",
              "path": "${idf}",
              "idfToolsPath": "${idf}/tools",
              "python": os.path.realpath("${idf}/python-env") + "/bin/python3",
              "activationScript": activate,
          }
          installed = [e for e in data.get("idfInstalled", []) if e.get("id") != entry_id]
          data.setdefault("version", "1.0")
          data.setdefault("gitPath", "${pkgs.git}/bin/git")
          data["idfInstalled"] = installed + [entry]
          data.setdefault("idfSelectedId", entry_id)
          with open(path + ".tmp", "w") as f:
              json.dump(data, f, indent=2)
          os.replace(path + ".tmp", path)
        '';
      in
      {
        devShells.default = pkgs.mkShell {
          # esp-idf-full: ESP-IDF plus every toolchain for all targets
          # (xtensa + riscv32 GCC, esp-clang, ULP, OpenOCD, GDB, ROM ELFs),
          # idf.py, esptool and the rest of the IDF Python environment.
          packages = [
            idf
            pkgs.fish
            pkgs.gh
          ];

          shellHook = ''
            ${toolEnvExports}
            ${registerIdf} "$PWD" ${idfActivate} || echo "warning: could not register ESP-IDF for VS Code" >&2
          '';
        };
      }
    );
}
