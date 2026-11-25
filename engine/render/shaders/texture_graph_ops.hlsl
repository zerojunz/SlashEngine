// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
// A texture graph pass's ops (slash/render/texture_graph.hpp), included once a pass with the pass
// described by macros; texture_graph_common.hlsl comes first. run_texture_graph in
// src/texture_graph.cpp is the CPU twin: a change to an op goes in both.
//
// TG(name)                 the pass's own name for each function (name, or name_p<n>)
// TG_OP                    the pass's op, or -1 for every op (then TG_OP_VALUE picks at run time)
// TG_OP_VALUE              the op, a uint
// TG_PARAM(i)              param i, a float
// TG_WIDTH, TG_HEIGHT      the size the graph bakes at, ints
// TG_TILING                whether the graph wraps at its edges, a bool
// TG_HAS_INPUT(k)          whether the pass reads input k, a bool
// TG_INPUT(k, x, y)        input k at a pixel inside the graph, a float4
// TG_IMAGE_WIDTH, TG_IMAGE_HEIGHT, TG_IMAGE(x, y)  the image an image pass samples

float TG(param)(uint i) {
    return TG_PARAM(i);
}

int TG(width_of)() {
    return TG_WIDTH;
}

int TG(height_of)() {
    return TG_HEIGHT;
}

int TG(wrap_x)(int x) {
    int w = TG(width_of)();
    if (TG_TILING) {
        return tg_wrap_int(x, w);
    }
    return clamp(x, 0, w - 1);
}

int TG(wrap_y)(int y) {
    int h = TG(height_of)();
    if (TG_TILING) {
        return tg_wrap_int(y, h);
    }
    return clamp(y, 0, h - 1);
}

// Input k at a pixel, wrapped or clamped; white where the pass leaves it out.
float4 TG(at)(uint k, int x, int y) {
    if (!TG_HAS_INPUT(k)) {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    return TG_INPUT(k, TG(wrap_x)(x), TG(wrap_y)(y));
}

#if TG_OP < 0 || TG_OP == 15 || TG_OP == 16
// Input k bilinearly at a UV point.
float4 TG(sample_in)(uint k, float u, float v) {
    float fx = u * (float)TG(width_of)() - 0.5;
    float fy = v * (float)TG(height_of)() - 0.5;
    float x0f = floor(fx);
    float y0f = floor(fy);
    float tx = fx - x0f;
    float ty = fy - y0f;
    int x0 = (int)x0f;
    int y0 = (int)y0f;
    float4 a = float4(0.0, 0.0, 0.0, 0.0);
    float4 b = a;
    float4 c = a;
    float4 d = a;
    // Each tap from one call site, so a live graph's shader holds the input once.
    [loop] for (uint i = 0u; i < 4u; ++i) {
        float4 t = TG(at)(k, x0 + (int)(i & 1u), y0 + (int)(i >> 1u));
        if (i == 0u) {
            a = t;
        } else if (i == 1u) {
            b = t;
        } else if (i == 2u) {
            c = t;
        } else {
            d = t;
        }
    }
    return tg_mix4(tg_mix4(a, b, tx), tg_mix4(c, d, tx), ty);
}
#endif

#if TG_OP < 0 || TG_OP == 7
float4 TG(image_texel)(int x, int y) {
    int w = TG_IMAGE_WIDTH;
    int h = TG_IMAGE_HEIGHT;
    if (TG_TILING) {
        x = tg_wrap_int(x, w);
        y = tg_wrap_int(y, h);
    } else {
        x = clamp(x, 0, w - 1);
        y = clamp(y, 0, h - 1);
    }
    return TG_IMAGE(x, y);
}

float4 TG(image_sample)(float u, float v) {
    float fx = u * (float)TG_IMAGE_WIDTH - 0.5;
    float fy = v * (float)TG_IMAGE_HEIGHT - 0.5;
    float x0f = floor(fx);
    float y0f = floor(fy);
    float tx = fx - x0f;
    float ty = fy - y0f;
    int x0 = (int)x0f;
    int y0 = (int)y0f;
    float4 a = TG(image_texel)(x0, y0);
    float4 b = TG(image_texel)(x0 + 1, y0);
    float4 c = TG(image_texel)(x0, y0 + 1);
    float4 d = TG(image_texel)(x0 + 1, y0 + 1);
    return tg_mix4(tg_mix4(a, b, tx), tg_mix4(c, d, tx), ty);
}
#endif

#if TG_OP < 0 || TG_OP == 2 || TG_OP == 11
float4 TG(ramp_point)(uint i) {
    uint at_word = kTgRampAt + 1u + i * 5u;
    return float4(TG(param)(at_word + 1u), TG(param)(at_word + 2u), TG(param)(at_word + 3u),
                  TG(param)(at_word + 4u));
}

float TG(ramp_offset)(uint i) {
    return TG(param)(kTgRampAt + 1u + i * 5u);
}

// The ramp in the params, as a Gradient with linear interpolation in sRGB.
float4 TG(ramp)(float offset) {
    int count = clamp(tg_to_int(TG(param)(kTgRampAt)), 0, (int)kTgRampPoints);
    if (count == 0) {
        return float4(offset, offset, offset, 1.0);
    }
    if (offset <= TG(ramp_offset)(0u)) {
        return TG(ramp_point)(0u);
    }
    for (int i = 1; i < count; ++i) {
        float b = TG(ramp_offset)((uint)i);
        if (offset < b) {
            float a = TG(ramp_offset)((uint)(i - 1));
            float t = b > a ? (offset - a) / (b - a) : 0.0;
            return tg_mix4(TG(ramp_point)((uint)(i - 1)), TG(ramp_point)((uint)i), t);
        }
    }
    return TG(ramp_point)((uint)(count - 1));
}
#endif

#if TG_OP < 0 || TG_OP == 1
fnl_state TG(noise_state)() {
    fnl_state state = fnlCreateState(tg_to_int(TG(param)(1u)));
    state.noise_type = tg_to_int(TG(param)(0u));
    state.frequency = TG(param)(2u);
    state.fractal_type = tg_to_int(TG(param)(3u));
    state.octaves = tg_to_int(TG(param)(4u));
    state.lacunarity = TG(param)(5u);
    state.gain = TG(param)(6u);
    state.weighted_strength = TG(param)(7u);
    state.ping_pong_strength = TG(param)(8u);
    state.cellular_distance_func = tg_to_int(TG(param)(9u));
    state.cellular_return_type = tg_to_int(TG(param)(10u));
    state.cellular_jitter_mod = TG(param)(11u);
    return state;
}

fnl_state TG(warp_state)() {
    fnl_state state = fnlCreateState(tg_to_int(TG(param)(1u)));
    state.domain_warp_type = tg_to_int(TG(param)(13u));
    state.domain_warp_amp = TG(param)(14u);
    state.frequency = TG(param)(15u);
    // The domain warp fractal type: none, progressive, independent.
    int fractal = tg_to_int(TG(param)(16u));
    state.fractal_type = fractal == 1   ? FNL_FRACTAL_DOMAIN_WARP_PROGRESSIVE
                         : fractal == 2 ? FNL_FRACTAL_DOMAIN_WARP_INDEPENDENT
                                        : FNL_FRACTAL_NONE;
    state.octaves = tg_to_int(TG(param)(17u));
    state.lacunarity = TG(param)(18u);
    state.gain = TG(param)(19u);
    return state;
}

// The offset added, the domain warped, then the noise.
float TG(noise_at)(fnl_state noise, fnl_state warp, float x, float y) {
    x += TG(param)(20u);
    y += TG(param)(21u);
#if FNL_USED_WARP != 0
    if (TG(param)(12u) > 0.5) {
        fnlDomainWarp2D(warp, x, y);
    }
#endif
    return fnlGetNoise2D(noise, x, y);
}
#endif

#if TG_OP < 0 || TG_OP == 1
// The noise pass's value at a pixel: placed as the seamless image places it when seamless (the CPU
// twin's seamless_noise), without its 8-bit steps. The places it reads are gathered first so the
// noise is sampled at one call site, which keeps the shader small.
float TG(noise_value)(int x, int y) {
    fnl_state noise = TG(noise_state)();
    fnl_state warp = TG(warp_state)();
    int width = TG(width_of)();
    int height = TG(height_of)();
    float sx = TG(param)(23u) / (float)width;
    float sy = TG(param)(24u) / (float)height;
    bool seamless = TG(param)(25u) > 0.5;
    float skirt = TG(param)(26u);
    int skirt_w = (int)max(1.0, (float)width * skirt);
    int skirt_h = (int)max(1.0, (float)height * skirt);
    int half_w = width / 2;
    int half_h = height / 2;
    int src_w = width + skirt_w;
    int src_h = height + skirt_h;
    bool in_x = seamless && x >= half_w && x < half_w + skirt_w;
    bool in_y = seamless && y >= half_h && y < half_h + skirt_h;
    // Without seams, [0] is the pixel's own place. The source image read with each mode's modulo:
    // [0] both alternate, [1] x alternate, [2] y alternate, [3] neither. One call site in a loop
    // kept as a loop, and no array it indexes, so a live graph's shader holds the noise once rather
    // than four times.
    float v0 = 0.0;
    float v1 = 0.0;
    float v2 = 0.0;
    float v3 = 0.0;
    [loop] for (uint i = 0u; i < 4u; ++i) {
        bool alt_x = i == 0u || i == 1u;
        bool alt_y = i == 0u || i == 2u;
        bool needed = i == 0u || (i == 1u && in_x) || (i == 2u && in_y) ||
                      (i == 3u && in_x && in_y);
        if (needed) {
            int mx = alt_x ? width : src_w;
            int my = alt_y ? height : src_h;
            int sxp = seamless ? (x + half_w) % mx : x;
            int syp = seamless ? (y + half_h) % my : y;
            float value = TG(noise_at)(noise, warp, (float)sxp * sx, (float)syp * sy);
            if (i == 0u) {
                v0 = value;
            } else if (i == 1u) {
                v1 = value;
            } else if (i == 2u) {
                v2 = value;
            } else {
                v3 = value;
            }
        }
    }
    if (in_x && in_y) {
        float wx = tg_skirt_weight(x, half_w, skirt_w);
        float wy = tg_skirt_weight(y, half_h, skirt_h);
        float top = tg_mix1(v3, v1, wx);
        float bottom = tg_mix1(v2, v0, wx);
        return tg_mix1(top, bottom, wy);
    }
    if (in_x) {
        return tg_mix1(v0, v1, tg_skirt_weight(x, half_w, skirt_w));
    }
    if (in_y) {
        return tg_mix1(v0, v2, tg_skirt_weight(y, half_h, skirt_h));
    }
    return v0;
}
#endif

#if TG_OP < 0 || TG_OP == 2
// GradientTexture2D::_get_gradient_offset_at.
float TG(gradient_offset)(float u, float v) {
    float fx = TG(param)(1u);
    float fy = TG(param)(2u);
    float tx = TG(param)(3u);
    float ty = TG(param)(4u);
    if (fx == tx && fy == ty) {
        return 0.0;
    }
    float pos = 0.0;
    int fill = tg_to_int(TG(param)(0u));
    if (fill == 1) {
        float dx = u - fx;
        float dy = v - fy;
        float rx = tx - fx;
        float ry = ty - fy;
        pos = sqrt(dx * dx + dy * dy) / sqrt(rx * rx + ry * ry);
    } else if (fill == 2) {
        float dx = abs(u - fx);
        float dy = abs(v - fy);
        float rx = abs(tx - fx);
        float ry = abs(ty - fy);
        pos = max(dx, dy) / max(rx, ry);
    } else {
        float rx = tx - fx;
        float ry = ty - fy;
        pos = ((u - fx) * rx + (v - fy) * ry) / (rx * rx + ry * ry);
    }
    int repeat = tg_to_int(TG(param)(5u));
    if (repeat == 1) {
        pos = tg_fract(pos);
    } else if (repeat == 2) {
        pos = abs(pos - 2.0 * floor(pos * 0.5 + 0.5));
    } else {
        pos = tg_clamp01(pos);
    }
    return pos;
}
#endif

#if TG_OP < 0 || TG_OP == 3
float TG(shape_value)(float u, float v) {
    float cells_x = max(1.0, TG(param)(1u));
    float cells_y = max(1.0, TG(param)(2u));
    float x = tg_fract(u * cells_x) - 0.5;
    float y = tg_fract(v * cells_y) - 0.5;
    float angle_turn = TG(param)(6u) * kTgTurn;
    float c = cos(angle_turn);
    float s = sin(angle_turn);
    float rx = x * c + y * s;
    float ry = -x * s + y * c;
    x = rx;
    y = ry;
    float size = max(1e-4, TG(param)(3u)) * 0.5;
    float soft = max(1e-4, TG(param)(5u) * size);
    float d = 0.0;
    int kind = tg_to_int(TG(param)(0u));
    if (kind == 1) {
        d = max(abs(x), abs(y)) - size;
    } else if (kind == 2) {
        float sides = max(3.0, floor(TG(param)(4u)));
        float angle = atan2(x, -y);
        float sector = kTgTurn / sides;
        float a = angle - sector * floor(angle / sector + 0.5);
        d = cos(a) * sqrt(x * x + y * y) - size * cos(sector * 0.5);
    } else if (kind == 3) {
        float r = sqrt(x * x + y * y);
        float half_ring = max(1e-4, TG(param)(7u)) * size * 0.5;
        d = abs(r - (size - half_ring)) - half_ring;
    } else {
        d = sqrt(x * x + y * y) - size;
    }
    return 1.0 - tg_smooth(-soft, 0.0, d);
}
#endif

#if TG_OP < 0 || TG_OP == 4
float4 TG(bricks)(float u, float v) {
    float across = max(1.0, TG(param)(0u));
    float down = max(1.0, TG(param)(1u));
    float by = v * down;
    float row = floor(by);
    float bx = u * across + TG(param)(3u) * row;
    float col = floor(bx);
    float fx = bx - col;
    float fy = by - row;
    float aspect = down / across;
    float ex = min(fx, 1.0 - fx) * (1.0 / max(aspect, 1e-4));
    float ey = min(fy, 1.0 - fy);
    float edge = min(ex * aspect, ey);
    float mortar = TG(param)(2u) * 0.5;
    float bevel = max(1e-4, TG(param)(4u));
    float height = tg_clamp01((edge - mortar) / bevel);
    uint wrapped_col = (uint)((int)col - (int)across * (int)floor(col / across));
    uint row_id = (uint)((int)row - (int)down * (int)floor(row / down));
    uint id = wrapped_col + row_id * 4096u;
    uint seed = (uint)tg_to_int(TG(param)(5u));
    return float4(edge > mortar ? height : 0.0, tg_hash01(id ^ tg_pcg_hash(seed)),
                  (float)(id % 1024u) / 1023.0, 1.0);
}
#endif

#if TG_OP < 0 || TG_OP == 6
float TG(waves)(float u, float v) {
    float angle = TG(param)(1u) * kTgTurn;
    float t = (u * cos(angle) + v * sin(angle)) * TG(param)(0u) + TG(param)(3u);
    int kind = tg_to_int(TG(param)(2u));
    if (kind == 1) {
        return 1.0 - abs(tg_fract(t) * 2.0 - 1.0);
    }
    if (kind == 2) {
        return tg_fract(t) < 0.5 ? 1.0 : 0.0;
    }
    return 0.5 + 0.5 * sin(tg_fract(t) * kTgTurn);
}
#endif

float4 TG(run_pixel)(int x, int y) {
    float u = ((float)x + 0.5) / (float)TG(width_of)();
    float v = ((float)y + 0.5) / (float)TG(height_of)();
    uint op = TG_OP_VALUE;
#if TG_OP < 0 || TG_OP == 0
    if (op == 0u) {
        return float4(TG(param)(0u), TG(param)(1u), TG(param)(2u), TG(param)(3u));
    }
#endif
#if TG_OP < 0 || TG_OP == 1
    if (op == 1u) {
        return tg_gray(TG(noise_value)(x, y) * 0.5 + 0.5);
    }
#endif
#if TG_OP < 0 || TG_OP == 2
    if (op == 2u) {
        return TG(ramp)(TG(gradient_offset)(u, v));
    }
#endif
#if TG_OP < 0 || TG_OP == 3
    if (op == 3u) {
        return tg_gray(TG(shape_value)(u, v));
    }
#endif
#if TG_OP < 0 || TG_OP == 4
    if (op == 4u) {
        return TG(bricks)(u, v);
    }
#endif
#if TG_OP < 0 || TG_OP == 5
    if (op == 5u) {
        int cx = (int)floor(u * max(1.0, TG(param)(0u)));
        int cy = (int)floor(v * max(1.0, TG(param)(1u)));
        return tg_gray(((cx + cy) & 1) != 0 ? 1.0 : 0.0);
    }
#endif
#if TG_OP < 0 || TG_OP == 6
    if (op == 6u) {
        return tg_gray(TG(waves)(u, v));
    }
#endif
#if TG_OP < 0 || TG_OP == 7
    if (op == 7u) {
        return TG(image_sample)(u, v);
    }
#endif
#if TG_OP < 0 || TG_OP == 8
    if (op == 8u) {
        float4 c = TG(at)(0u, x, y);
        float range = max(1e-6, TG(param)(1u) - TG(param)(0u));
        float gamma = max(1e-4, TG(param)(2u));
        for (uint k = 0u; k < 3u; ++k) {
            float t = tg_clamp01((c[k] - TG(param)(0u)) / range);
            float curved = gamma == 1.0 ? t : (t > 0.0 ? exp(log(t) / gamma) : 0.0);
            c[k] = tg_mix1(TG(param)(3u), TG(param)(4u), curved);
        }
        return c;
    }
#endif
#if TG_OP < 0 || TG_OP == 9
    if (op == 9u) {
        float4 c = TG(at)(0u, x, y);
        return float4(1.0 - c.xyz, c.w);
    }
#endif
#if TG_OP < 0 || TG_OP == 10
    if (op == 10u) {
        return clamp(TG(at)(0u, x, y), 0.0, 1.0);
    }
#endif
#if TG_OP < 0 || TG_OP == 11
    if (op == 11u) {
        return TG(ramp)(tg_clamp01(tg_luminance(TG(at)(0u, x, y))));
    }
#endif
#if TG_OP < 0 || TG_OP == 12
    if (op == 12u) {
        return tg_hsv_adjust(TG(at)(0u, x, y), TG(param)(0u), TG(param)(1u), TG(param)(2u));
    }
#endif
#if TG_OP < 0 || TG_OP == 13
    if (op == 13u) {
        int radius = clamp(tg_to_int(TG(param)(0u)), 0, 256);
        bool across = tg_to_int(TG(param)(1u)) == 0;
        float4 sum = float4(0.0, 0.0, 0.0, 0.0);
        float total = 0.0;
        [loop] for (int d = -radius; d <= radius; ++d) {
            float t = (float)d / (float)(radius + 1);
            float w0 = 1.0 - t * t;
            float w = w0 * w0;
            float4 c = across ? TG(at)(0u, x + d, y) : TG(at)(0u, x, y + d);
            sum += c * w;
            total += w;
        }
        return sum / total;
    }
#endif
#if TG_OP < 0 || TG_OP == 14
    if (op == 14u) {
        float4 c = TG(at)(0u, x, y);
        float4 around = (TG(at)(0u, x - 1, y) + TG(at)(0u, x + 1, y) + TG(at)(0u, x, y - 1) +
                         TG(at)(0u, x, y + 1)) * 0.25;
        return float4(c.xyz + TG(param)(0u) * (c.xyz - around.xyz), c.w);
    }
#endif
#if TG_OP < 0 || TG_OP == 15
    if (op == 15u) {
        float du = 0.0;
        float dv = 0.0;
        if (tg_to_int(TG(param)(1u)) == 1) {
            float angle = TG(param)(2u) * kTgTurn;
            float g = TG(at)(1u, x, y).x;
            du = cos(angle) * g * TG(param)(0u);
            dv = sin(angle) * g * TG(param)(0u);
        } else {
            float gx = tg_luminance(TG(at)(1u, x + 1, y)) - tg_luminance(TG(at)(1u, x - 1, y));
            float gy = tg_luminance(TG(at)(1u, x, y + 1)) - tg_luminance(TG(at)(1u, x, y - 1));
            du = gx * 0.5 * TG(param)(0u);
            dv = gy * 0.5 * TG(param)(0u);
        }
        return TG(sample_in)(0u, u + du, v + dv);
    }
#endif
#if TG_OP < 0 || TG_OP == 16
    if (op == 16u) {
        float angle = TG(param)(2u) * kTgTurn;
        float c = cos(angle);
        float s = sin(angle);
        float cu = u - 0.5;
        float cv = v - 0.5;
        float ru = cu * c + cv * s;
        float rv = -cu * s + cv * c;
        float su = ru * TG(param)(0u) + 0.5 + TG(param)(3u);
        float sv = rv * TG(param)(1u) + 0.5 + TG(param)(4u);
        if (TG_TILING) {
            su = tg_fract(su);
            sv = tg_fract(sv);
        }
        return TG(sample_in)(0u, su, sv);
    }
#endif
#if TG_OP < 0 || TG_OP == 17
    if (op == 17u) {
        float l00 = tg_luminance(TG(at)(0u, x - 1, y - 1));
        float l10 = tg_luminance(TG(at)(0u, x, y - 1));
        float l20 = tg_luminance(TG(at)(0u, x + 1, y - 1));
        float l01 = tg_luminance(TG(at)(0u, x - 1, y));
        float l21 = tg_luminance(TG(at)(0u, x + 1, y));
        float l02 = tg_luminance(TG(at)(0u, x - 1, y + 1));
        float l12 = tg_luminance(TG(at)(0u, x, y + 1));
        float l22 = tg_luminance(TG(at)(0u, x + 1, y + 1));
        float gx = (l20 + 2.0 * l21 + l22) - (l00 + 2.0 * l01 + l02);
        float gy = (l02 + 2.0 * l12 + l22) - (l00 + 2.0 * l10 + l20);
        return tg_gray(sqrt(gx * gx + gy * gy) * TG(param)(0u));
    }
#endif
#if TG_OP < 0 || TG_OP == 18
    if (op == 18u) {
        float4 c = TG(at)(0u, x, y);
        float g = tg_luminance(c);
        return float4(g, g, g, c.w);
    }
#endif
#if TG_OP < 0 || TG_OP == 19
    if (op == 19u) {
        float4 c = TG(at)(0u, x, y);
        float g = tg_smooth(TG(param)(0u) - TG(param)(1u), TG(param)(0u) + TG(param)(1u), tg_luminance(c));
        return float4(g, g, g, c.w);
    }
#endif
#if TG_OP < 0 || TG_OP == 20
    if (op == 20u) {
        float4 c = TG(at)(0u, x, y);
        float steps = max(1.0, floor(TG(param)(0u)));
        return float4(floor(clamp(c.xyz, 0.0, 1.0) * steps + 0.5) / steps, c.w);
    }
#endif
#if TG_OP < 0 || TG_OP == 21
    if (op == 21u) {
        float4 merged = float4(0.0, 0.0, 0.0, 0.0);
        [loop] for (uint k = 0u; k < 4u; ++k) {
            float m = !TG_HAS_INPUT(k) ? TG(param)(4u + k)
                                       : tg_channel_of(TG(at)(k, x, y), tg_to_int(TG(param)(k)));
            if (k == 0u) {
                merged.x = m;
            } else if (k == 1u) {
                merged.y = m;
            } else if (k == 2u) {
                merged.z = m;
            } else {
                merged.w = m;
            }
        }
        return merged;
    }
#endif
#if TG_OP < 0 || TG_OP == 22
    if (op == 22u) {
        float4 c = TG(at)(0u, x, y);
        return float4(tg_channel_of(c, tg_to_int(TG(param)(0u))), tg_channel_of(c, tg_to_int(TG(param)(1u))),
                      tg_channel_of(c, tg_to_int(TG(param)(2u))), tg_channel_of(c, tg_to_int(TG(param)(3u))));
    }
#endif
#if TG_OP < 0 || TG_OP == 23
    if (op == 23u) {
        // The four neighbors (right, left, down, up) from one call site.
        float r = 0.0;
        float l = 0.0;
        float dn = 0.0;
        float up = 0.0;
        [loop] for (uint k = 0u; k < 4u; ++k) {
            int ox = k == 0u ? 1 : (k == 1u ? -1 : 0);
            int oy = k == 2u ? 1 : (k == 3u ? -1 : 0);
            float t = TG(at)(0u, x + ox, y + oy).x;
            if (k == 0u) {
                r = t;
            } else if (k == 1u) {
                l = t;
            } else if (k == 2u) {
                dn = t;
            } else {
                up = t;
            }
        }
        float dx = (r - l) * 0.5 * TG(param)(0u);
        float dy = (dn - up) * 0.5 * TG(param)(0u);
        float nx = -dx;
        float ny = dy;
        float len = sqrt(nx * nx + ny * ny + 1.0);
        return float4(0.5 + 0.5 * nx / len, 0.5 + 0.5 * ny / len, 0.5 + 0.5 / len, 1.0);
    }
#endif
#if TG_OP < 0 || TG_OP == 24
    if (op == 24u) {
        float h = TG(at)(0u, x, y).x;
        int radius = clamp(tg_to_int(TG(param)(0u)), 1, 64);
        float occlusion = 0.0;
        // Right, left, down, up, then the diagonals (1, 1), (-1, -1), (1, -1), (-1, 1).
        [loop] for (uint k = 0u; k < 8u; ++k) {
            int2 dir = k == 0u ? int2(1, 0)
                     : k == 1u ? int2(-1, 0)
                     : k == 2u ? int2(0, 1)
                     : k == 3u ? int2(0, -1)
                     : k == 4u ? int2(1, 1)
                     : k == 5u ? int2(-1, -1)
                     : k == 6u ? int2(1, -1)
                               : int2(-1, 1);
            float most = 0.0;
            [loop] for (int step = 1; step <= radius; ++step) {
                float rise = TG(at)(0u, x + dir.x * step, y + dir.y * step).x - h;
                most = max(most, rise * (float)radius / (float)step);
            }
            occlusion += tg_clamp01(most);
        }
        return tg_gray(tg_clamp01(1.0 - occlusion * 0.125 * TG(param)(1u)));
    }
#endif
#if TG_OP < 0 || TG_OP == 25
    if (op == 25u) {
        // The four neighbors and the pixel from one call site, summed in the order they were.
        float r = 0.0;
        float l = 0.0;
        float dn = 0.0;
        float up = 0.0;
        float here = 0.0;
        [loop] for (uint k = 0u; k < 5u; ++k) {
            int ox = k == 0u ? 1 : (k == 1u ? -1 : 0);
            int oy = k == 2u ? 1 : (k == 3u ? -1 : 0);
            float t = TG(at)(0u, x + ox, y + oy).x;
            if (k == 0u) {
                r = t;
            } else if (k == 1u) {
                l = t;
            } else if (k == 2u) {
                dn = t;
            } else if (k == 3u) {
                up = t;
            } else {
                here = t;
            }
        }
        float lap = r + l + dn + up - 4.0 * here;
        return tg_gray(tg_clamp01(0.5 - lap * TG(param)(0u)));
    }
#endif
#if TG_OP < 0 || TG_OP == 26
    if (op == 26u) {
        float4 a = TG(at)(0u, x, y);
        float4 b = TG(at)(1u, x, y);
        float mask = TG(at)(2u, x, y).x;
        float t = tg_clamp01(TG(param)(1u) * mask * b.w);
        int mode = tg_to_int(TG(param)(0u));
        float4 blended = a;
        for (uint k = 0u; k < 3u; ++k) {
            blended[k] = tg_mix1(a[k], tg_blend_channel(mode, a[k], b[k]), t);
        }
        return blended;
    }
#endif
#if TG_OP < 0 || TG_OP == 27
    if (op == 27u) {
        float4 a = TG(at)(0u, x, y);
        float4 b = !TG_HAS_INPUT(1u) ? float4(TG(param)(2u), TG(param)(3u), TG(param)(4u), TG(param)(5u))
                                       : TG(at)(1u, x, y);
        int which = tg_to_int(TG(param)(0u));
        float4 result;
        for (uint k = 0u; k < 4u; ++k) {
            result[k] = tg_math_channel(which, a[k], b[k]);
        }
        return result;
    }
#endif
#if TG_OP < 0 || TG_OP == 28
    if (op == 28u) {
        float4 c = TG(at)(0u, x, y);
        float range = TG(param)(1u) - TG(param)(0u);
        for (uint k = 0u; k < 3u; ++k) {
            float t = range != 0.0 ? (c[k] - TG(param)(0u)) / range : 0.0;
            if (TG(param)(4u) > 0.5) {
                t = tg_clamp01(t);
            }
            c[k] = tg_mix1(TG(param)(2u), TG(param)(3u), t);
        }
        return c;
    }
#endif
    return float4(0.0, 0.0, 0.0, 1.0);
}
