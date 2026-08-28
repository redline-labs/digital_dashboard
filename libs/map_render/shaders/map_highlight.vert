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
    float pxPerLocal;
    float widthScale;
    float fadeAlpha;
    float extraHalfPx;
    vec4 highlight;
};

const float kMinHalfPx = 0.5;

void main() {
    float screenHalf = halfPx * widthScale;

    // The extra is added AFTER the zoom taper, so the casing is a constant lip
    // in screen pixels at every zoom -- the same rule bridge casings follow.
    // No alpha compensation for the widening, unlike map.vert's hairline fade:
    // the highlight is a solid overlay whose weight IS the point, not ink
    // whose total must be preserved.
    float drawnHalf = max(screenHalf + extraHalfPx, kMinHalfPx);

    vec2 p = pos + nrm * (drawnHalf / max(pxPerLocal, 1e-6));
    // The fragment stage takes its colour from the uniform; vcol only keeps
    // the interface identical to map.vert's.
    vcol = vec4(col.rgb, col.a);
    gl_Position = mvp * vec4(p, 0.0, 1.0);
}
