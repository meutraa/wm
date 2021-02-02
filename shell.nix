{ pkgs ? import <nixpkgs> { } }:
with pkgs;
let
  enableXWayland = true;
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
    xorg.libxcb
    x11
  ];
}
