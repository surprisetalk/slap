{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } { buildInputs = with pkgs; [ autoconf automake libtool pkg-config gnumake gcc m4 SDL2 SDL2_image SDL2_mixer SDL2_ttf libGL libGLU glew freetype zlib readline git python3 which bison flex xorg.libX11 xorg.xorgproto libglvnd mesa ]; }
