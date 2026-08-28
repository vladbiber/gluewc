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
            ]
            # Attributes that were renamed: the newer name first, the older one
            # as a fallback, so the flake evaluates on either nixpkgs.
            ++ [
              (pkgs.libxcb or pkgs.xorg.libxcb)
              (pkgs.libxcb-wm or pkgs.xorg.xcbutilwm)
              (pkgs.wlroots_0_20 or pkgs.wlroots)
              (pkgs.scenefx_0_5 or pkgs.scenefx)
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

      # The module runs on NixOS and on finix. finix has no systemd, so it
      # carries programs.pipewire, services.rtkit and services.polkit where
      # NixOS carries services.pipewire, security.rtkit and security.polkit,
      # and it has no display-manager session registry at all. Every one of
      # those is set only when the running system declares it, so the same
      # module evaluates on both.
      nixosModules.default =
        {
          config,
          options,
          pkgs,
          lib,
          ...
        }:
        let
          cfg = config.programs.gluewc;
          declares = path: lib.hasAttrByPath path options;
          setIfDeclared =
            path: value: lib.optionalAttrs (declares path) (lib.setAttrByPath path value);
          # Whichever spelling this system has, or nothing at all.
          setFirstDeclared =
            paths: value:
            let
              found = lib.findFirst declares null paths;
            in
            if found == null then { } else lib.setAttrByPath found value;
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

          config = lib.mkIf cfg.enable (
            lib.mkMerge [
              {
                # The package carries share/wayland-sessions/gluewc.desktop,
                # which is all a greeter reading XDG_DATA_DIRS needs.
                environment.systemPackages = [ cfg.package ];
              }
              (setIfDeclared [ "services" "displayManager" "sessionPackages" ] [ cfg.package ])
              (setIfDeclared [ "programs" "xwayland" "enable" ] (lib.mkDefault true))
              (setIfDeclared [ "hardware" "graphics" "enable" ] (lib.mkDefault true))
              (setIfDeclared [ "fonts" "enableDefaultPackages" ] (lib.mkDefault true))
              (setFirstDeclared [
                [ "security" "polkit" "enable" ]
                [ "services" "polkit" "enable" ]
              ] true)
              (setIfDeclared [ "xdg" "portal" ] {
                enable = lib.mkDefault true;
                extraPortals = with pkgs; [
                  xdg-desktop-portal-wlr
                  xdg-desktop-portal-gtk
                ];
                config.gluewc.default = lib.mkDefault [
                  "wlr"
                  "gtk"
                ];
              })
              (lib.mkIf cfg.audio (
                lib.mkMerge [
                  (setFirstDeclared [
                    [ "security" "rtkit" "enable" ]
                    [ "services" "rtkit" "enable" ]
                  ] (lib.mkDefault true))
                  # gluewc-session starts the daemons itself, so a system
                  # without user services still gets sound from this.
                  (setFirstDeclared [
                    [ "services" "pipewire" ]
                    [ "programs" "pipewire" ]
                  ] {
                    enable = lib.mkDefault true;
                    alsa.enable = lib.mkDefault true;
                    pulse.enable = lib.mkDefault true;
                  })
                ]
              ))
            ]
          );
        };
    };
}
