{ pkgs ? ((import <nixpkgs> { })) }:
with pkgs;
let
  version = "b6ba595862a55e5e2899d4c38dd22a1f8ffcabaa";

  wlroots-git = wlroots.overrideAttrs (old: {
    version = version;
    src = fetchFromGitHub {
      owner = "swaywm";
      repo = "wlroots";
      rev = version;
      sha256 = "0jv2z6gjic24n5i2h3j4q02cy6dm2bbppjf9a79rj3dgxdfkbv0n";
    };
    buildInputs = old.buildInputs ++ [ libuuid ];
  });

in stdenv.mkDerivation rec {
  pname = "wm";
  version = "0.2";
  src = lib.cleanSource ./.;

  nativeBuildInputs = [ wayland-protocols pkg-config ];

  installPhase = ''
    mkdir -p $out/bin
    cp wm $out/bin
  '';

  buildInputs = [
    libGL
    libinput
    libxkbcommon
    pkgsStatic.pixman
    wayland
    wlroots-git
    pkgsStatic.xorg.libxcb
    pkgsStatic.x11
  ];
}
