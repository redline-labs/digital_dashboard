#version 450

// The highlight pass's vertex stage: map.vert with the line widened by
// extraHalfPx. A separate stage rather than a branch in map.vert because the
// widening must apply ONLY here -- the base pass has already drawn the road at
// its own width, and this pass draws over it as a casing.
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 nrm;
layout(location = 2) in float halfPx;
layout(location = 3) in vec4 col;

layout(location = 0) out vec4 vcol;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec2 viewportPx;
    float widthScale;
    float fadeAlpha;
    float extraHalfPx;
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
    float screenHalf = halfPx * widthScale;

    // The extra is added AFTER the zoom taper, so the casing is a constant lip
    // in screen pixels at every zoom -- the same rule bridge casings follow.
    // No alpha compensation for the widening, unlike map.vert's hairline fade:
    // the highlight is a solid overlay whose weight IS the point, not ink
    // whose total must be preserved.
    float drawnHalf = max(screenHalf + extraHalfPx, kMinHalfPx);

    // The fragment stage takes its colour from the uniform; vcol only keeps
    // the interface identical to map.vert's.
    vcol = vec4(col.rgb, col.a);
    gl_Position = expandToScreenWidth(pos, nrm, drawnHalf, mvp, viewportPx);
}
