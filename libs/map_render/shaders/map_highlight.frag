#version 450

// The highlight pass: the same geometry as the map, in one colour.
//
// A second FRAGMENT shader rather than a second vertex format, because what is
// being highlighted is geometry already on the GPU. The route a driver is
// following and the road they are on come back from map/nearest and map/route
// as OSM way ids, and map_build stamps that id on every tile feature precisely
// so a client can find the road again -- so highlighting is recolouring what is
// already uploaded, not overlaying a second polyline that can drift away from
// the road it is meant to be on.
//
// It shares the vertex shader, and therefore the vertex buffer, the uniform
// block and the line expansion. Only the colour differs, and it comes from the
// per-tile uniform rather than the vertex.
layout(location = 0) in vec4 vcol;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec2 viewportPx;
    float widthScale;
    float fadeAlpha;
    float extraHalfPx;
    vec4 highlight;
};

void main() {
    // vcol is read so the input stays live: dropping it would leave the vertex
    // shader writing an output nothing consumes, and some backends warn or
    // relink around that. Multiplying by zero keeps the interface identical to
    // map.frag's.
    //
    // fadeAlpha, so a highlight rides its tile's crossfade instead of sitting
    // at full strength over a road that is still fading in.
    fragColor = vec4(highlight.rgb, highlight.a * fadeAlpha) + (vcol * 0.0);
}
