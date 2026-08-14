#version 450

// Vertices arrive in TILE-LOCAL coordinates, [0,1] across the tile. That is
// what makes them cacheable: they do not depend on the camera, on neighbouring
// tiles, or on the zoom, so panning re-uploads nothing. It is also what keeps
// them precise -- absolute Web Mercator world coordinates at z14 are ~0.17 with
// a pixel worth 1.2e-7, which is right at the edge of float32 and shows up as
// vertex jitter when the camera moves.
layout(location = 0) in vec2 pos;
// Perpendicular for line expansion, unit length in local space. (0,0) on fills.
layout(location = 1) in vec2 nrm;
// Half line width in SCREEN pixels at full width. 0 on fills.
layout(location = 2) in float halfPx;
layout(location = 3) in vec4 col;

layout(location = 0) out vec4 vcol;

layout(std140, binding = 0) uniform buf {
    // tile-local [0,1] -> clip. Carries the tile's placement, the map rotation
    // and the projection, so a camera move is this matrix and nothing else.
    mat4 mvp;
    // Screen pixels across one local unit, i.e. the tile's on-screen size.
    float pxPerLocal;
    // Road widths shrink as you zoom out. A uniform rather than baked into the
    // vertex, so zooming does not invalidate the geometry.
    float widthScale;
    vec2 pad;
};

void main() {
    // Expanding here rather than on the CPU is what keeps a road the same
    // number of pixels wide at every zoom AND under rotation: the normal is in
    // local units and the matrix rotates it, which preserves its length.
    vec2 p = pos + nrm * (halfPx * widthScale / max(pxPerLocal, 1e-6));
    vcol = col;
    gl_Position = mvp * vec4(p, 0.0, 1.0);
}
