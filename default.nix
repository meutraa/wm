{ pkgs ? ((import <nixpkgs> { })) }:
with pkgs;
let
  version = "3d7aa7386706f6aa8041f27a3fba22d5b4290e82";

  wlroots-git = wlroots.overrideAttrs (old: {
    version = version;
    src = fetchFromGitHub {
      owner = "swaywm";
      repo = "wlroots";
      rev = version;
      sha256 = "1z98zyqarp3g7cv5mf8qhlawav42dxlglm6r3lvw4b6k6ql3ij60";
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
    cp main $out/bin/wm
  '';

  buildInputs =
    [ libGL libinput libxkbcommon pixman wayland wlroots-git xorg.libxcb x11 ];
}
