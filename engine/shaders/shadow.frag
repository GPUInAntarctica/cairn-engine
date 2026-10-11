#version 450
// Depth-only cascade pass: the rendering instance declares no color
// attachment, so this stage writes nothing. It still exists because a
// graphics pipeline needs a fragment stage, and an empty main() compiles to a
// valid (if trivial) module.
void main() {}
