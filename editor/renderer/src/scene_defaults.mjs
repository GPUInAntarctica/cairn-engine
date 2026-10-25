// Cairn Engine editor - values shared by new scenes and the project templates.
//
// The 3D template's scene file is data, not code, so this module cannot be its
// only source; test/editor.js compares the two and fails if they drift.

// A directional light's forward vector is the direction the light travels, so
// the sun looks down when pitch is negative.
export const DEFAULT_SUN_ROTATION = [-55, -130, 0];
