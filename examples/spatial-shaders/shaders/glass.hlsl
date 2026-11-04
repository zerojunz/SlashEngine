// See-through glass: writing ALPHA makes the mesh transparent.
void fragment() {
    ALBEDO = tint.rgb;
    ROUGHNESS = 0.05;
    ALPHA = tint.a;
}
