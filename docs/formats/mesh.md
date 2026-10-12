# `.mesh`: baked static segment geometry

File = zlib stream (zlib header `78 9c`). Decompressed, little endian:

```
u32  vertexCount N
N x  Vertex (24 bytes):
       f32 x, y, z        position (segment-local; the loader adds the segment's z offset in the room)
       f32 u, v           texcoord into gfx/tiles.png (8x8 tiles of 128 px, v grows downward)
       u8  r, g, b, a     vertex colour: r,g,b = tint * directional light * 0.5 ; a = ambient occlusion
u32  triangleCount T
T x  3 x u32              vertex indices (triangle list)
```
Total size = 8 + 24N + 12T. Geometry is quads split into two triangles
(T == N/2 in practice). All 643 meshes in Smash Hit 1.5.14 follow this layout.
The original engine stores the indices as 16-bit, enough for the largest mesh's 64096 vertices.
Each segment `foo.xml` has a sibling `foo.mesh`: the XML lists the
physics boxes/obstacles, the mesh is only the visible static geometry.
Triangles are counter-clockwise (front faces toward the viewer; only faces that can be seen from
the +z side/inside the corridor exist, e.g. `-z` faces are absent), and the room is drawn with
back-face culling.

## Vertex colour decode (verified from `room.glsl` and `chasms.mesh`)
* **r,g,b** (0..255) = `floor(templateColor * lightFactor(faceNormal) * 0.5 * 255)`. The factor 0.5 exists
  because the shader multiplies by 2 (`vColor = vec4(aColor.rgb, 0.5) * 2*(1-fog)`), so template colours
  above 1.0 (e.g. 1.3, 1.75) fit in a byte. `lightFactor` = template property by face normal:
  `+x -> lightRight`, `-x -> lightLeft`, `+y -> lightTop`, `-y -> lightBottom`, `+z -> lightFront`,
  `-z -> lightBack`. Example (`basic_s` + `basic`: color `.8 .91 .96`; left 1, right .8, top 1, bottom .7,
  front .88): top/-x faces `(102,116,122)`, +x `(81,92,97)`, -y `(71,81,85)`, +z `(89,102,107)`.
* **a** = baked ambient occlusion, 255 = fully lit (computed by Mediocre's level editor from 64 hemisphere rays,
  `1 - sqrt(clamp(sum(1 - hit/range)/64, 0, 1))`). `room.glsl` uses `light = 1 - (1-a)^2`; `roomlow.glsl`
  uses `min(1, a + 0.3)`. It is *not* an opacity.
* UVs: tile index `i` -> `u = (i%8)/8`, `v = (i/8)/8`, with a 1/256 inset per side (tile 19 gives
  `u in [0.379, 0.496]`, `v in [0.254, 0.371]`). Larger faces are tiled by the template `tileSize`/`tileRot`.
* At load the engine multiplies G by 0.7 and B by 0.6 in Expert mode (player mode 2).
