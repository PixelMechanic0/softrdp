#include "blender.h"

void rdp_blender_decode(rdp_blender_program *p, uint32_t w1)
{
    if (!p) return;
    p->cycle[0] = (rdp_blender_cycle){ (w1 >> 30) & 3u, (w1 >> 26) & 3u,
                                       (w1 >> 22) & 3u, (w1 >> 18) & 3u };
    p->cycle[1] = (rdp_blender_cycle){ (w1 >> 28) & 3u, (w1 >> 24) & 3u,
                                       (w1 >> 20) & 3u, (w1 >> 16) & 3u };
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

static uint8_t factor_a(uint8_t code, const rdp_blend_state *s,
                        rdp_color pixel, uint8_t shade_alpha)
{
    switch (code & 3u) {
    case 0: return pixel.a;
    case 1: return s->fog_color.a;
    case 2: return shade_alpha;
    default:return 0u;
    }
}

static uint8_t factor_b(uint8_t code, uint8_t a, rdp_color memory)
{
    switch (code & 3u) {
    case 0: return (uint8_t)~a;
    case 1: return memory.a;
    case 2: return 0xffu;
    default:return 0u;
    }
}

static rdp_color evaluate_cycle(const rdp_blend_state *s,
                                const rdp_blender_cycle *c,
                                rdp_color pixel, rdp_color memory,
                                uint8_t shade_alpha,
                                bool final_cycle)
{
    const rdp_color x = color_source(c->color_a, s, pixel, memory);
    const rdp_color y = color_source(c->color_b, s, pixel, memory);
    const uint32_t a = factor_a(c->factor_a, s, pixel, shade_alpha) >> 3;
    const uint32_t b = (factor_b(c->factor_b, (uint8_t)(a << 3), memory) >> 3) + 1u;
    const uint32_t divisor = (!final_cycle || s->force_blend) ? 32u : a + b;
    if (!divisor) return x;
    return (rdp_color){
        (uint8_t)(((uint32_t)x.r * a + (uint32_t)y.r * b) / divisor),
        (uint8_t)(((uint32_t)x.g * a + (uint32_t)y.g * b) / divisor),
        (uint8_t)(((uint32_t)x.b * a + (uint32_t)y.b * b) / divisor),
        pixel.a
    };
}

rdp_color rdp_blender_evaluate(const rdp_blend_state *s, rdp_color pixel,
                               rdp_color memory, uint8_t shade_alpha,
                               bool blend_enable)
{
    if (!s) return pixel;
    const rdp_blender_cycle *final = s->cycle_type == RDP_CYCLE_2
        ? &s->program.cycle[1] : &s->program.cycle[0];
    if (!blend_enable)
        return color_source(final->color_a, s, pixel, memory);
    if (s->cycle_type == RDP_CYCLE_2) {
        pixel = evaluate_cycle(s, &s->program.cycle[0], pixel, memory, shade_alpha, false);
        return evaluate_cycle(s, &s->program.cycle[1], pixel, memory, shade_alpha, true);
    }
    return evaluate_cycle(s, &s->program.cycle[0], pixel, memory, shade_alpha, true);
}
