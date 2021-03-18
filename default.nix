{ pkgs ? ((import <nixpkgs> { })) }:
with pkgs;
let
  version = "c740fccc9dd0913908c0632c10f8c6d10b2b1ca4";

  wlroots-git = wlroots.overrideAttrs (old: {
    version = version;
    src = fetchFromGitHub {
      owner = "swaywm";
      repo = "wlroots";
      rev = version;
      sha256 = "0667qvrfp338cmswaykvga487bql0xhisndwcq50n7xcjlib1lxj";
    };
    buildInputs = old.buildInputs ++ [ libuuid xorg.xcbutilrenderutil xwayland ];
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
