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
      sha256 = "1ljrv8glahf5x32z5lab6xf6ngc3c3ji7mix2sgrzz5pk008fnx1";
    };
    buildInputs = old.buildInputs ++ [ libuuid xorg.xcbutilrenderutil ];
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

  buildInputs = [ libGL libinput libxkbcommon pixman wayland wlroots-git x11 ];
}
