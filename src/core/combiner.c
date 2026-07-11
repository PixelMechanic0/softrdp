#include "combiner.h"

#include <string.h>

static rdp_combiner_source decode_rgb_a(uint32_t code)
{
    static const uint8_t sources[8] = {
        RDP_COMBINER_COMBINED_RGB, RDP_COMBINER_TEXEL0_RGB,
        RDP_COMBINER_TEXEL1_RGB, RDP_COMBINER_PRIMITIVE_RGB,
        RDP_COMBINER_SHADE_RGB, RDP_COMBINER_ENVIRONMENT_RGB,
        RDP_COMBINER_ONE, RDP_COMBINER_NOISE
    };
    return code < 8u ? (rdp_combiner_source)sources[code] : RDP_COMBINER_ZERO;
}

static rdp_combiner_source decode_rgb_b(uint32_t code)
{
    static const uint8_t sources[8] = {
        RDP_COMBINER_COMBINED_RGB, RDP_COMBINER_TEXEL0_RGB,
        RDP_COMBINER_TEXEL1_RGB, RDP_COMBINER_PRIMITIVE_RGB,
        RDP_COMBINER_SHADE_RGB, RDP_COMBINER_ENVIRONMENT_RGB,
        RDP_COMBINER_KEY_CENTER, RDP_COMBINER_K4
    };
    return code < 8u ? (rdp_combiner_source)sources[code] : RDP_COMBINER_ZERO;
}

static rdp_combiner_source decode_rgb_c(uint32_t code)
{
    static const uint8_t sources[16] = {
        RDP_COMBINER_COMBINED_RGB, RDP_COMBINER_TEXEL0_RGB,
        RDP_COMBINER_TEXEL1_RGB, RDP_COMBINER_PRIMITIVE_RGB,
        RDP_COMBINER_SHADE_RGB, RDP_COMBINER_ENVIRONMENT_RGB,
        RDP_COMBINER_KEY_SCALE, RDP_COMBINER_COMBINED_ALPHA,
        RDP_COMBINER_TEXEL0_ALPHA, RDP_COMBINER_TEXEL1_ALPHA,
        RDP_COMBINER_PRIMITIVE_ALPHA, RDP_COMBINER_SHADE_ALPHA,
        RDP_COMBINER_ENVIRONMENT_ALPHA, RDP_COMBINER_LOD_FRACTION,
        RDP_COMBINER_PRIMITIVE_LOD_FRACTION, RDP_COMBINER_K5
    };
    return code < 16u ? (rdp_combiner_source)sources[code] : RDP_COMBINER_ZERO;
}

static rdp_combiner_source decode_rgb_d(uint32_t code)
{
    static const uint8_t sources[8] = {
        RDP_COMBINER_COMBINED_RGB, RDP_COMBINER_TEXEL0_RGB,
        RDP_COMBINER_TEXEL1_RGB, RDP_COMBINER_PRIMITIVE_RGB,
        RDP_COMBINER_SHADE_RGB, RDP_COMBINER_ENVIRONMENT_RGB,
        RDP_COMBINER_ONE, RDP_COMBINER_ZERO
    };
    return (rdp_combiner_source)sources[code & 7u];
}

static rdp_combiner_source decode_alpha_abd(uint32_t code)
{
    static const uint8_t sources[8] = {
        RDP_COMBINER_COMBINED_ALPHA, RDP_COMBINER_TEXEL0_ALPHA,
        RDP_COMBINER_TEXEL1_ALPHA, RDP_COMBINER_PRIMITIVE_ALPHA,
        RDP_COMBINER_SHADE_ALPHA, RDP_COMBINER_ENVIRONMENT_ALPHA,
        RDP_COMBINER_ONE, RDP_COMBINER_ZERO
    };
    return (rdp_combiner_source)sources[code & 7u];
}

static rdp_combiner_source decode_alpha_c(uint32_t code)
{
    static const uint8_t sources[8] = {
        RDP_COMBINER_LOD_FRACTION, RDP_COMBINER_TEXEL0_ALPHA,
        RDP_COMBINER_TEXEL1_ALPHA, RDP_COMBINER_PRIMITIVE_ALPHA,
        RDP_COMBINER_SHADE_ALPHA, RDP_COMBINER_ENVIRONMENT_ALPHA,
        RDP_COMBINER_PRIMITIVE_LOD_FRACTION, RDP_COMBINER_ZERO
    };
    return (rdp_combiner_source)sources[code & 7u];
}

static uint8_t source_mask(rdp_combiner_source source)
{
    switch (source) {
    case RDP_COMBINER_TEXEL0_RGB:
    case RDP_COMBINER_TEXEL0_ALPHA: return RDP_COMBINER_INPUT_TEXEL0;
    case RDP_COMBINER_TEXEL1_RGB:
    case RDP_COMBINER_TEXEL1_ALPHA: return RDP_COMBINER_INPUT_TEXEL1;
    case RDP_COMBINER_SHADE_RGB:
    case RDP_COMBINER_SHADE_ALPHA:  return RDP_COMBINER_INPUT_SHADE;
    default:                        return 0u;
    }
}

static void finish_program(rdp_combiner_program *program)
{
    program->input_mask = 0u;
    for (uint32_t cycle = 0; cycle < 2u; cycle++) {
        const uint8_t *sources = (const uint8_t *)&program->cycle[cycle];
        for (uint32_t i = 0; i < sizeof(program->cycle[cycle]); i++) {
            program->input_mask |= source_mask((rdp_combiner_source)sources[i]);
        }
    }
}

void rdp_combiner_decode(rdp_combiner_program *program, uint32_t w0, uint32_t w1)
{
    if (!program) return;
    rdp_combiner_cycle *c0 = &program->cycle[0];
    rdp_combiner_cycle *c1 = &program->cycle[1];

    c0->rgb_a = decode_rgb_a((w0 >> 20) & 0xfu);
    c0->rgb_c = decode_rgb_c((w0 >> 15) & 0x1fu);
    c0->alpha_a = decode_alpha_abd((w0 >> 12) & 7u);
    c0->alpha_c = decode_alpha_c((w0 >> 9) & 7u);
    c1->rgb_a = decode_rgb_a((w0 >> 5) & 0xfu);
    c1->rgb_c = decode_rgb_c(w0 & 0x1fu);
    c0->rgb_b = decode_rgb_b((w1 >> 28) & 0xfu);
    c1->rgb_b = decode_rgb_b((w1 >> 24) & 0xfu);
    c1->alpha_a = decode_alpha_abd((w1 >> 21) & 7u);
    c1->alpha_c = decode_alpha_c((w1 >> 18) & 7u);
    c0->rgb_d = decode_rgb_d((w1 >> 15) & 7u);
    c0->alpha_b = decode_alpha_abd((w1 >> 12) & 7u);
    c0->alpha_d = decode_alpha_abd((w1 >> 9) & 7u);
    c1->rgb_d = decode_rgb_d((w1 >> 6) & 7u);
    c1->alpha_b = decode_alpha_abd((w1 >> 3) & 7u);
    c1->alpha_d = decode_alpha_abd(w1 & 7u);
    finish_program(program);
}

void rdp_combiner_make_passthrough(rdp_combiner_program *program,
                                   rdp_combiner_source rgb,
                                   rdp_combiner_source alpha)
{
    if (!program) return;
    memset(program, 0, sizeof(*program));
    for (uint32_t i = 0; i < 2u; i++) {
        program->cycle[i].rgb_a = (uint8_t)rgb;
        program->cycle[i].rgb_c = RDP_COMBINER_ONE;
        program->cycle[i].alpha_a = (uint8_t)alpha;
        program->cycle[i].alpha_c = RDP_COMBINER_ONE;
    }
    finish_program(program);
}

typedef struct combiner_value { int32_t r, g, b, a; } combiner_value;

static int32_t source_component(rdp_combiner_source source,
                                const rdp_combiner_inputs *in,
                                const combiner_value *combined,
                                uint32_t component)
{
    const rdp_color *color = NULL;
    switch (source) {
    case RDP_COMBINER_COMBINED_RGB:     return component == 0 ? combined->r : component == 1 ? combined->g : combined->b;
    case RDP_COMBINER_COMBINED_ALPHA:   return combined->a;
    case RDP_COMBINER_TEXEL0_RGB:       color = &in->texel0; break;
    case RDP_COMBINER_TEXEL0_ALPHA:     return in->texel0.a;
    case RDP_COMBINER_TEXEL1_RGB:       color = &in->texel1; break;
    case RDP_COMBINER_TEXEL1_ALPHA:     return in->texel1.a;
    case RDP_COMBINER_PRIMITIVE_RGB:    color = &in->primitive; break;
    case RDP_COMBINER_PRIMITIVE_ALPHA:  return in->primitive.a;
    case RDP_COMBINER_SHADE_RGB:        color = &in->shade; break;
    case RDP_COMBINER_SHADE_ALPHA:      return in->shade.a;
    case RDP_COMBINER_ENVIRONMENT_RGB:  color = &in->environment; break;
    case RDP_COMBINER_ENVIRONMENT_ALPHA:return in->environment.a;
    case RDP_COMBINER_LOD_FRACTION:     return in->lod_fraction;
    case RDP_COMBINER_PRIMITIVE_LOD_FRACTION: return in->primitive_lod_fraction;
    case RDP_COMBINER_ONE:              return 0x100;
    default:                            return 0;
    }
    return component == 0 ? color->r : component == 1 ? color->g : color->b;
}

static int32_t extend_9(int32_t value)
{
    value &= 0x1ff;
    return (value & 0x180) == 0x180 ? value | ~0x1ff : value;
}

static int32_t rgb_equation(int32_t a, int32_t b, int32_t c, int32_t d)
{
    return ((extend_9(a) - extend_9(b)) * extend_9(c) + extend_9(d) * 256 + 0x80) & 0x1ffff;
}

static int32_t alpha_equation(int32_t a, int32_t b, int32_t c, int32_t d)
{
    return (((extend_9(a) - extend_9(b)) * extend_9(c) + extend_9(d) * 256 + 0x80) >> 8) & 0x1ff;
}

static uint8_t clamp_9(int32_t value)
{
    value &= 0x1ff;
    switch ((value >> 7) & 3) {
    case 0: case 1: return (uint8_t)(value & 0xff);
    case 2:         return 0xffu;
    default:        return 0u;
    }
}

static void evaluate_cycle(const rdp_combiner_cycle *cycle,
                           const rdp_combiner_inputs *inputs,
                           combiner_value *combined)
{
    combiner_value next;
    for (uint32_t component = 0; component < 3u; component++) {
        const int32_t raw = rgb_equation(source_component((rdp_combiner_source)cycle->rgb_a, inputs, combined, component),
                                         source_component((rdp_combiner_source)cycle->rgb_b, inputs, combined, component),
                                         source_component((rdp_combiner_source)cycle->rgb_c, inputs, combined, component),
                                         source_component((rdp_combiner_source)cycle->rgb_d, inputs, combined, component));
        if (component == 0) next.r = (raw >> 8) & 0x1ff;
        else if (component == 1) next.g = (raw >> 8) & 0x1ff;
        else next.b = (raw >> 8) & 0x1ff;
    }
    next.a = alpha_equation(source_component((rdp_combiner_source)cycle->alpha_a, inputs, combined, 3u),
                            source_component((rdp_combiner_source)cycle->alpha_b, inputs, combined, 3u),
                            source_component((rdp_combiner_source)cycle->alpha_c, inputs, combined, 3u),
                            source_component((rdp_combiner_source)cycle->alpha_d, inputs, combined, 3u));
    *combined = next;
}

rdp_color rdp_combiner_evaluate(const rdp_combiner_program *program,
                                rdp_cycle_type cycle_type,
                                const rdp_combiner_inputs *inputs)
{
    if (!program || !inputs) return (rdp_color){0, 0, 0, 0};
    combiner_value combined = {0, 0, 0, 0};
    if (cycle_type == RDP_CYCLE_2) evaluate_cycle(&program->cycle[0], inputs, &combined);
    evaluate_cycle(&program->cycle[1], inputs, &combined);
    return (rdp_color){ clamp_9(combined.r), clamp_9(combined.g), clamp_9(combined.b), clamp_9(combined.a) };
}
