#version 450
// Raw authored color, no tonemap - matches the software line path byte for
// byte (it writes c*255 straight into the framebuffer).
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(vColor.rgb, 1.0); }
