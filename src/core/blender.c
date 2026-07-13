#include "blender.h"

static uint8_t compile_cycle(const rdp_blender_cycle *cycle)
{
    /* These are the two general RDP fog equations. Keeping them as compiled
     * blender operations removes four selector branches from every fogged
     * fragment without creating a separate rendering path. */
    if (cycle->color_a == 3u && cycle->factor_a == 2u &&
        cycle->color_b == 0u && cycle->factor_b == 0u)
        return RDP_BLEND_OP_FOG_SHADE_ALPHA;
    if (cycle->color_a == 0u && cycle->factor_a == 1u &&
        cycle->color_b == 3u && cycle->factor_b == 0u)
        return RDP_BLEND_OP_FOG_ALPHA;
    return RDP_BLEND_OP_GENERIC;
}

static uint8_t cycle_input_mask(const rdp_blender_cycle *cycle)
{
    uint8_t mask = 0u;
    if (cycle->color_a == 1u || cycle->color_b == 1u || cycle->factor_b == 1u)
        mask |= RDP_BLENDER_INPUT_MEMORY;
    if (cycle->color_a == 3u || cycle->color_b == 3u || cycle->factor_a == 1u)
        mask |= RDP_BLENDER_INPUT_FOG;
    if (cycle->factor_a == 2u)
        mask |= RDP_BLENDER_INPUT_SHADE_ALPHA;
    return mask;
}

void rdp_blender_decode(rdp_blender_program *p, uint32_t w1)
{
    if (!p) return;
    p->cycle[0] = (rdp_blender_cycle){ (w1 >> 30) & 3u, (w1 >> 26) & 3u,
                                       (w1 >> 22) & 3u, (w1 >> 18) & 3u };
    p->cycle[1] = (rdp_blender_cycle){ (w1 >> 28) & 3u, (w1 >> 24) & 3u,
                                       (w1 >> 20) & 3u, (w1 >> 16) & 3u };
    for (uint32_t cycle = 0; cycle < 2u; cycle++) {
        p->operation[cycle] = compile_cycle(&p->cycle[cycle]);
        p->input_mask[cycle] = cycle_input_mask(&p->cycle[cycle]);
    }
}

static rdp_color color_source(uint8_t code, const rdp_blend_state *s,
                              rdp_color pixel, rdp_color memory)
{
    switch (code & 3u) {
    case 0: return pixel;
    case 1: return memory;
    case 2: return s->blend_color;
    default:return s->fog_color;
    }
}

static uint16_t factor_a(uint8_t code, const rdp_blend_state *s,
                         uint16_t pixel_alpha, uint8_t shade_alpha)
{
    switch (code & 3u) {
    case 0: return pixel_alpha;
    case 1: return s->fog_color.a;
    case 2: return shade_alpha;
    default:return 0u;
    }
}

static uint16_t factor_b(uint8_t code, uint16_t a, rdp_color memory)
{
    switch (code & 3u) {
    case 0: return a >= 0x100u ? 0u : 0xffu - a;
    case 1: return memory.a;
    case 2: return 0xffu;
    default:return 0u;
    }
}

static rdp_color evaluate_cycle(const rdp_blend_state *s,
                                const rdp_blender_cycle *c,
                                rdp_blender_op operation,
                                rdp_color pixel, uint16_t pixel_alpha,
                                rdp_color memory,
                                uint8_t shade_alpha,
                                bool final_cycle)
{
    rdp_color x;
    rdp_color y;
    uint16_t raw_a;
    switch (operation) {
    case RDP_BLEND_OP_FOG_SHADE_ALPHA:
        x = s->fog_color;
        y = pixel;
        raw_a = shade_alpha;
        break;
    case RDP_BLEND_OP_FOG_ALPHA:
        x = pixel;
        y = s->fog_color;
        raw_a = s->fog_color.a;
        break;
    default:
        x = color_source(c->color_a, s, pixel, memory);
        y = color_source(c->color_b, s, pixel, memory);
        raw_a = factor_a(c->factor_a, s, pixel_alpha, shade_alpha);
        break;
    }
    if (final_cycle && c->factor_a == 0u && c->factor_b == 0u &&
        pixel_alpha >= 0xffu)
        return x;
    const uint32_t a = raw_a >> 3;
    const uint32_t b = (factor_b(c->factor_b, raw_a, memory) >> 3) + 1u;
    const uint32_t red = (uint32_t)x.r * a + (uint32_t)y.r * b;
    const uint32_t green = (uint32_t)x.g * a + (uint32_t)y.g * b;
    const uint32_t blue = (uint32_t)x.b * a + (uint32_t)y.b * b;
    if (!final_cycle || s->force_blend) {
        return (rdp_color){
            (uint8_t)(red >> 5),
            (uint8_t)(green >> 5),
            (uint8_t)(blue >> 5),
            pixel.a
        };
    }
    const uint32_t divisor = a + b;
    if (!divisor) return x;
    return (rdp_color){
        (uint8_t)(red / divisor),
        (uint8_t)(green / divisor),
        (uint8_t)(blue / divisor),
        pixel.a
    };
}

rdp_color rdp_blender_evaluate(const rdp_blend_state *s, rdp_color pixel,
                               uint16_t pixel_alpha, rdp_color memory,
                               uint8_t shade_alpha,
                               bool blend_enable)
{
    if (!s) return pixel;
    const rdp_blender_cycle *final = &s->program.cycle[s->final_cycle];
    if (s->cycle_count == 2u) {
        /* Cycle 0 is part of the two-cycle color pipeline (commonly fog), not
         * conditional framebuffer blending. Only cycle 1 may be bypassed by
         * blend_enable. */
        pixel = evaluate_cycle(s, &s->program.cycle[0],
                               (rdp_blender_op)s->program.operation[0], pixel, pixel_alpha,
                               memory, shade_alpha, false);
        if (!blend_enable)
            return color_source(final->color_a, s, pixel, memory);
        return evaluate_cycle(s, &s->program.cycle[1],
                              (rdp_blender_op)s->program.operation[1], pixel, pixel_alpha,
                              memory, shade_alpha, true);
    }
    if (!blend_enable)
        return color_source(final->color_a, s, pixel, memory);
    return evaluate_cycle(s, &s->program.cycle[0],
                          (rdp_blender_op)s->program.operation[0], pixel, pixel_alpha,
                          memory, shade_alpha, true);
}
