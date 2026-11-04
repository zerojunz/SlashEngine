// Cel shading, as Godot's light(): each light's diffuse light cut into flat steps, a sharp
// highlight, and a rim of light where the surface turns away from the camera.
void fragment() {
    ALBEDO = tint.rgb;
    ROUGHNESS = 0.5;
}

void light() {
    // NORMAL, LIGHT and VIEW are in view space, as Godot's; ATTENUATION has the shadows in it.
    float lit = saturate(dot(NORMAL, LIGHT)) * ATTENUATION;
    float stepped = floor(lit * steps + 0.5) / steps;
    DIFFUSE_LIGHT += stepped * LIGHT_COLOR / PI;
    float3 half_way = normalize(LIGHT + VIEW);
    float shine = step(0.97, dot(NORMAL, half_way)) * ATTENUATION;
    SPECULAR_LIGHT += shine * LIGHT_COLOR * SPECULAR_AMOUNT * 0.25;
    if (LIGHT_IS_DIRECTIONAL) {
        float rim = step(0.75, 1.0 - saturate(dot(NORMAL, VIEW))) * stepped;
        DIFFUSE_LIGHT += rim * rim_color.rgb * LIGHT_COLOR / PI;
    }
}
