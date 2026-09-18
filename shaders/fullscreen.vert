#version 450

// Fullscreen triangle from the vertex index alone. No vertex buffer, no input assembly state,
// and one triangle rather than two so the diagonal seam cannot cause redundant shading.

layout(location = 0) out vec2 vUV;

void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
