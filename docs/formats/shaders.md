# Shaders

`shaders/*.glsl`: GLSL ES 1.00 (`attribute`/`varying`/`texture2D`). One file holds both stages:
`#ifdef VERTEX … #endif` and `#ifdef FRAGMENT … #endif`; the loader prepends `#define VERTEX` or
`#define FRAGMENT`. Common attributes: `aPosition`, `aTexCoord`, `aColor`; uniforms prefixed `u`
(`uMvpMatrix`, `uTexture0`, `uLowerFog`, `uUpperFog`). Fog is computed per vertex (see `basicglass.glsl`).
OpenSmash prepends `#version 330 core` and translates the GLES-only syntax at load time (`src/render/shader.cpp`).
