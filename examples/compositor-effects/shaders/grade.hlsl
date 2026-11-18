// Tints the world and darkens it toward the corners, after the blended meshes.
void fragment() {
    float2 from_center = SCREEN_UV - 0.5;
    float fade = saturate(1.0 - vignette * dot(from_center, from_center));
    COLOR.rgb *= tint.rgb * fade;
}
