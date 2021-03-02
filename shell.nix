{ pkgs ? import <nixpkgs> { } }:
with pkgs;
let
  enableXWayland = true;
  version = "5e19e0053a5800252a27ce127fd015d641ee2e9e";

  wlroots-git = wlroots.overrideAttrs (old: {
    version = version;
    src = fetchFromGitHub {
      owner = "swaywm";
      repo = "wlroots";
      rev = version;
      sha256 = "1ljrv8glahf5x32z5lab6xf6ngc3c3ji7mix2sgrzz5pk008fnx1";
    };
    buildInputs = old.buildInputs ++ [ 
      libuuid 
      xorg.xcbutilrenderutil
    ];
  });
in pkgs.mkShell {
  name = "wm-env";
  nativeBuildInputs = [ pkg-config ];
  buildInputs = [
    libGL
    libinput
    libxkbcommon
    pixman
    wayland
    wayland-protocols
    wlroots-git
    x11
  ];
}
