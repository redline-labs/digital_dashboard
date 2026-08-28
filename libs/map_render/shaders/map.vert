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
    // This tile's crossfade, 1.0 once it has settled. Multiplies EVERY alpha,
    // fills included -- unlike the hairline fade below, which must never touch
    // a fill. A tile fades in as a whole or the layers shear apart visibly.
    float fadeAlpha;
    // Extra half-width for the highlight pass, in screen pixels. Read only by
    // map_highlight.vert, but declared here because the block layout has to
    // match across every stage that binds it -- as are the two fields below.
    float extraHalfPx;
    // Read only by map_highlight.frag.
    vec4 highlight;
};

// Half a pixel, i.e. one pixel of total width, below which a line stops being
// reliably drawable. See main().
const float kMinHalfPx = 0.5;

void main() {
    // Half-width in SCREEN pixels, after the zoom taper.
    float screenHalf = halfPx * widthScale;

    // A line thinner than a pixel does not land on pixel centres reliably: it
    // breaks into dashes, and which dashes depends on sub-pixel position, so it
    // CRAWLS as the camera moves. That is the state minor roads are in at low
    // zoom -- widthScaleForZoom() tapers to 0.15, so a 1.5 px half-width is
    // 0.22 px, well under half a pixel.
    //
    // Widen to the floor and take the difference out of ALPHA. Total ink stays
    // the same, so the road keeps its weight in the picture, but it is now a
    // continuous faint line instead of an intermittent solid one. This is what
    // MSAA alone cannot do: four coverage samples cannot represent a fifth of a
    // pixel of coverage, and no sample count fixes a line that misses the pixel
    // entirely.
    //
    // Guarded on screenHalf > 0 so FILLS are untouched: they carry a zero
    // normal and a zero halfPx, and a fill must never be faded.
    float drawnHalf = max(screenHalf, kMinHalfPx);
    float fade = screenHalf > 0.0 ? screenHalf / drawnHalf : 1.0;

    // Expanding here rather than on the CPU is what keeps a road the same
    // number of pixels wide at every zoom AND under rotation: the normal is in
    // local units and the matrix rotates it, which preserves its length.
    vec2 p = pos + nrm * (drawnHalf / max(pxPerLocal, 1e-6));
    vcol = vec4(col.rgb, col.a * fade * fadeAlpha);
    gl_Position = mvp * vec4(p, 0.0, 1.0);
}
