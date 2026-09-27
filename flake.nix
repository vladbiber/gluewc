{
  description = "gluewc — an animated Wayland compositor with BSP, scrolling and infinite-canvas layouts";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  # The bar's QML tree, packaged here so a broken glueqs flake can never
  # break a gluewc rebuild; `nix flake update glueqs` moves it forward.
  inputs.glueqs = {
    url = "github:vladbiber/glueqs";
    flake = false;
  };

  outputs =
    {
      self,
      nixpkgs,
      glueqs,
    }:
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
      # withBar adds the bar to the default config the session seeds at first
      # login, which is how the module's bar.enable reaches a fresh account.
      mkGluewc =
        pkgs:
        {
          withBar ? false,
        }:
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

          # wayland-scanner is a build tool, so strictDeps puts its pkg-config
          # file on PKG_CONFIG_PATH_FOR_BUILD, where the pkg-config wrapper on
          # PATH cannot see it. The binary itself is right there on PATH, so
          # name it instead of asking pkg-config where it lives.
          makeFlags = [
            "PREFIX=$(out)"
            "SESSIONDIR=$(out)/share/wayland-sessions"
            "VERSION=${version}"
            "WAYLAND_SCANNER=wayland-scanner"
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
          ''
          + lib.optionalString withBar ''
            printf '\n# the glueqs bar\nautostart = glueqs\n' >> $out/share/gluewc/config.def.conf
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

      # glueqs: the QML tree under share/glueqs and a `glueqs` command that
      # runs it with the Quickshell from nixpkgs. Its settings live under
      # $XDG_CONFIG_HOME/glueqs, so the store path being read-only is fine.
      # The bar shells out to curl, bluetoothctl and loginctl; nmcli is only
      # used when NetworkManager is there, so it is not forced onto PATH.
      mkGlueqs =
        pkgs:
        pkgs.stdenvNoCC.mkDerivation {
          pname = "glueqs";
          version = "0-unstable-${glueqs.lastModifiedDate or "unknown"}";
          src = glueqs;
          nativeBuildInputs = [ pkgs.makeWrapper ];
          dontBuild = true;
          dontConfigure = true;
          installPhase = ''
            runHook preInstall
            mkdir -p $out/share/glueqs $out/bin
            cp -r . $out/share/glueqs
            rm -rf $out/share/glueqs/docs $out/share/glueqs/flake.nix $out/share/glueqs/flake.lock
            makeWrapper ${pkgs.quickshell}/bin/qs $out/bin/glueqs \
              --add-flags "-p $out/share/glueqs" \
              --prefix PATH : ${
                lib.makeBinPath (
                  with pkgs;
                  [
                    curl
                    bluez
                    coreutils
                  ]
                )
              }
            runHook postInstall
          '';
          meta = {
            description = "Dot-matrix desktop shell for gluewc, on Quickshell";
            homepage = "https://github.com/vladbiber/glueqs";
            license = lib.licenses.gpl3Only;
            mainProgram = "glueqs";
            platforms = lib.platforms.linux;
          };
        };
    in
    {
      packages = forAllSystems (pkgs: rec {
        gluewc = mkGluewc pkgs { };
        gluewc-with-bar = mkGluewc pkgs { withBar = true; };
        glueqs = mkGlueqs pkgs;
        default = gluewc;
      });

      overlays.default = final: _prev: {
        gluewc = mkGluewc final { };
        glueqs = mkGlueqs final;
      };

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ (mkGluewc pkgs { }) ];
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
              default =
                if cfg.bar.enable then
                  self.packages.${pkgs.stdenv.hostPlatform.system}.gluewc-with-bar
                else
                  self.packages.${pkgs.stdenv.hostPlatform.system}.gluewc;
              defaultText = lib.literalExpression "gluewc.packages.\${system}.gluewc";
              description = ''
                The gluewc package to install. With bar.enable the default
                is the same package whose seeded config autostarts glueqs.
              '';
            };

            bar = {
              enable = lib.mkEnableOption ''
                glueqs, the Quickshell bar written for gluewc. Installs the
                `glueqs` command with Quickshell and the tools its panels use,
                turns on Bluetooth and UPower, and seeds new accounts with
                `autostart = glueqs`. An existing ~/.config/gluewc/config.conf
                needs that line added by hand
              '';

              package = lib.mkOption {
                type = lib.types.package;
                default = self.packages.${pkgs.stdenv.hostPlatform.system}.glueqs;
                defaultText = lib.literalExpression "gluewc.packages.\${system}.glueqs";
                description = "The glueqs package to install.";
              };
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
              (lib.mkIf cfg.bar.enable (
                lib.mkMerge [
                  {
                    environment.systemPackages = [
                      cfg.bar.package
                    ]
                    ++ (with pkgs; [
                      curl
                      bluez
                      playerctl
                      brightnessctl
                      grim
                      slurp
                      wl-clipboard
                    ]);
                  }
                  (setIfDeclared [ "hardware" "bluetooth" "enable" ] (lib.mkDefault true))
                  (setIfDeclared [ "services" "upower" "enable" ] (lib.mkDefault true))
                ]
              ))
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
