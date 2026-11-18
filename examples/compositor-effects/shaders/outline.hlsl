// Inks the edges between things, found where the depth jumps or the prepass's normals turn.
void fragment() {
    float2 step = SCREEN_PIXEL_SIZE * width;
    float d = depth_texture(SCREEN_UV);
    float3 n = normal_roughness_texture(SCREEN_UV).xyz;
    float edge = 0.0;
    for (int i = 0; i < 4; ++i) {
        float2 offset = float2(i == 0 ? step.x : (i == 1 ? -step.x : 0.0),
                               i == 2 ? step.y : (i == 3 ? -step.y : 0.0));
        float other = depth_texture(SCREEN_UV + offset);
        float3 turned = normal_roughness_texture(SCREEN_UV + offset).xyz;
        edge = max(edge, saturate(abs(other - d) * 40.0));
        edge = max(edge, saturate((1.0 - dot(n, turned)) * 2.0));
    }
    COLOR.rgb = lerp(COLOR.rgb, ink.rgb, edge);
}
