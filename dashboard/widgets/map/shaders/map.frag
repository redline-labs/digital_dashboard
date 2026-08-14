#version 450

// Flat colour. Everything that varies -- per layer, per road class -- is baked
// into the vertex colour at tessellation time, so the whole map is one pipeline
// and the fragment stage does no work worth naming.
layout(location = 0) in vec4 vcol;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vcol;
}
