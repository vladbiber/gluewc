{
  description = "gluewc — an animated Wayland compositor with BSP, scrolling and infinite-canvas layouts";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      lib = nixpkgs.lib;
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # config.mk stays the single place the version is written down.
      version = lib.pipe (builtins.readFile ./config.mk) [
        (lib.splitString "\n")
        (lib.findFirst (lib.hasPrefix "_VERSION = ") "_VERSION = 0")
        (lib.removePrefix "_VERSION = ")
        (lib.removeSuffix " ")
      ];

      # BEGIN package
      mkGluewc =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "gluewc";
          inherit version;
          src = self;

          strictDeps = true;
          nativeBuildInputs = with pkgs; [
            pkg-config
            wayland-scanner
            makeWrapper
          ];

          buildInputs =
            with pkgs;
            [
              wayland
              wayland-protocols
              libinput
              libxkbcommon
              pixman
              libdrm
              libGL
              libgbm
              seatd
              wlroots_0_19
            ]
            # Attributes that were renamed: the newer name first, the older one
            # as a fallback, so the flake evaluates on either nixpkgs.
            ++ [
              (pkgs.libxcb or pkgs.xorg.libxcb)
              (pkgs.libxcb-wm or pkgs.xorg.xcbutilwm)
              (pkgs.scenefx_0_4 or pkgs.scenefx)
            ];

          makeFlags = [
            "PREFIX=$(out)"
            "SESSIONDIR=$(out)/share/wayland-sessions"
            "VERSION=${version}"
          ];

          # The session wrapper looks its helpers up on PATH: the compositor
          # itself, D-Bus, Xwayland and the audio daemons that give the session
          # sound without any further configuration.
          postInstall = ''
            wrapProgram $out/bin/gluewc-session \
              --set-default GLUEWC_DATADIR $out/share/gluewc \
              --prefix PATH : ${
                lib.makeBinPath (
                  with pkgs;
                  [
                    dbus
                    pipewire
                    wireplumber
                    xwayland
                    procps
                  ]
                )
              }:$out/bin
          '';

          passthru.providedSessions = [ "gluewc" ];

          meta = {
            description = "Animated Wayland compositor with BSP, scrolling and infinite-canvas layouts";
            homepage = "https://github.com/vladbiber/gluewc";
            license = lib.licenses.gpl3Only;
            mainProgram = "gluewc";
            platforms = lib.platforms.linux;
          };
        };
      # END package
    in
    {
      packages = forAllSystems (pkgs: rec {
        gluewc = mkGluewc pkgs;
        default = gluewc;
      });

      overlays.default = final: _prev: { gluewc = mkGluewc final; };

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ (mkGluewc pkgs) ];
          packages = with pkgs; [
            gdb
            foot
          ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-tree);

      nixosModules.default =
        {
          config,
          pkgs,
          lib,
          ...
        }:
        let
          cfg = config.programs.gluewc;
        in
        {
          options.programs.gluewc = {
            enable = lib.mkEnableOption "gluewc, an animated Wayland compositor";

            package = lib.mkOption {
              type = lib.types.package;
              default = self.packages.${pkgs.stdenv.hostPlatform.system}.gluewc;
              defaultText = lib.literalExpression "gluewc.packages.\${system}.gluewc";
              description = "The gluewc package to install.";
            };

            audio = lib.mkOption {
              type = lib.types.bool;
              default = true;
              description = ''
                Set up PipeWire with its ALSA and PulseAudio bridges, so the
                session has working sound without further configuration.
              '';
            };
          };

          config = lib.mkIf cfg.enable {
            environment.systemPackages = [ cfg.package ];
            services.displayManager.sessionPackages = [ cfg.package ];

            programs.xwayland.enable = lib.mkDefault true;
            hardware.graphics.enable = lib.mkDefault true;
            security.polkit.enable = true;
            fonts.enableDefaultPackages = lib.mkDefault true;

            xdg.portal = {
              enable = lib.mkDefault true;
              extraPortals = with pkgs; [
                xdg-desktop-portal-wlr
                xdg-desktop-portal-gtk
              ];
              config.gluewc.default = lib.mkDefault [
                "wlr"
                "gtk"
              ];
            };

            security.rtkit.enable = lib.mkIf cfg.audio (lib.mkDefault true);
            services.pipewire = lib.mkIf cfg.audio {
              enable = lib.mkDefault true;
              alsa.enable = lib.mkDefault true;
              pulse.enable = lib.mkDefault true;
            };
          };
        };
    };
}
