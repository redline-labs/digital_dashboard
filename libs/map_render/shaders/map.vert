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
    // The frame's size in DEVICE pixels. Line expansion happens after the
    // matrix now, and converting an NDC offset into pixels needs it.
    vec2 viewportPx;
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

// The projected centreline, offset by `drawnHalf` PIXELS along the projected
// normal.
//
// Expanding here -- after the matrix -- rather than in tile-local space is
// what keeps a road the same number of pixels wide under a PERSPECTIVE
// matrix. Local space cannot express it: the local-to-screen scale under
// pitch is neither constant across a tile (it falls off with distance) nor
// the same in x and y (the vertical is squeezed by cos(pitch) and a second
// factor of w), so no single per-tile scale factor stands in for it. Doing it
// per vertex also drops any dependence on the tile's own size, which is what
// makes a road cross a LOD boundary without a step in its width.
vec4 expandToScreenWidth(vec2 pos, vec2 nrm, float drawnHalf, mat4 mvp, vec2 viewportPx)
{
    vec4 centre = mvp * vec4(pos, 0.0, 1.0);

    // Fills carry a zero normal and must never be widened; a vertex behind the
    // eye has no screen position to offset from. Both are left where they are
    // -- the second is off-screen by construction and gets clipped.
    if (nrm == vec2(0.0) || centre.w <= 1e-6)
    {
        return centre;
    }

    // Where the normal POINTS on screen, by finite difference: project a short
    // step along it and subtract. One extra matrix multiply, and it is exact
    // for the affine (unpitched) case as well.
    const float kStep = 1.0 / 1024.0;
    vec4 stepped = mvp * vec4(pos + (nrm * kStep), 0.0, 1.0);
    vec2 halfViewport = viewportPx * 0.5;
    vec2 deltaPx =
        ((stepped.xy / max(stepped.w, 1e-6)) - (centre.xy / centre.w)) * halfViewport;

    float len = length(deltaPx);
    if (len < 1e-6)
    {
        return centre;
    }

    // Back into clip space. An offset in NDC has to be multiplied by w to
    // survive the perspective divide that follows.
    vec2 offsetNdc = ((deltaPx / len) * drawnHalf) / halfViewport;
    return vec4(centre.xy + (offsetNdc * centre.w), centre.z, centre.w);
}

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

    vcol = vec4(col.rgb, col.a * fade * fadeAlpha);
    gl_Position = expandToScreenWidth(pos, nrm, drawnHalf, mvp, viewportPx);
}
