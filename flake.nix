{
  description = "mGBA fork with online multiplayer support";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        mgba-online = pkgs.mgba.overrideAttrs (old: {
          pname = "mgba-online";
          version = "0.11.0-unstable-${self.lastModifiedDate or "dirty"}";

          src = pkgs.lib.cleanSource self;

          buildInputs =
            map (input: if input == pkgs.ffmpeg then pkgs.ffmpeg_7 else input) old.buildInputs
            ++ [ pkgs.asio ];
        });
      in
      {
        packages = {
          inherit mgba-online;
          default = mgba-online;
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ mgba-online ];

          inherit (mgba-online) cmakeFlags;
        };

        formatter = pkgs.nixfmt;
      }
    );
}
