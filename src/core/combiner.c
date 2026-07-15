#include "combiner.h"
#include <string.h>
#include <immintrin.h>

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
    case RDP_COMBINER_LOD_FRACTION: return RDP_COMBINER_INPUT_LOD_FRACTION;
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
    case RDP_COMBINER_KEY_CENTER:        color = &in->key_center; break;
    case RDP_COMBINER_KEY_SCALE:         color = &in->key_scale; break;
    case RDP_COMBINER_LOD_FRACTION:     return in->lod_fraction;
    case RDP_COMBINER_PRIMITIVE_LOD_FRACTION: return in->primitive_lod_fraction;
    case RDP_COMBINER_K4:               return in->k4;
    case RDP_COMBINER_K5:               return in->k5;
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

static inline __m256i load_extend_8(const uint16_t *ptr)
{
    __m128i val128 = _mm_loadu_si128((const __m128i *)ptr);
    return _mm256_cvtepu16_epi32(val128);
}

static inline void store_pack_8(uint16_t *dst, __m256i val)
{
    __m256i packed = _mm256_packus_epi32(val, val);
    __m128i lo = _mm256_castsi256_si128(packed);
    __m128i hi = _mm256_extracti128_si256(packed, 1);
    __m128i final_val = _mm_unpacklo_epi64(lo, hi);
    _mm_storeu_si128((__m128i *)dst, final_val);
}

static inline __m256i simde_extend_9(__m256i value)
{
    __m256i v = _mm256_and_si256(value, _mm256_set1_epi32(0x1ff));
    __m256i test = _mm256_and_si256(v, _mm256_set1_epi32(0x180));
    __m256i cond = _mm256_cmpeq_epi32(test, _mm256_set1_epi32(0x180));
    __m256i sign_extended = _mm256_or_si256(v, _mm256_set1_epi32(0xfffffe00));
    return _mm256_blendv_epi8(v, sign_extended, cond);
}

static inline __m256i simd_load_source(uint32_t source,
                                       const rdp_fragment_block *packet,
                                       const __m256i combined_in[4],
                                       const rdp_color_pipeline_state *state,
                                       uint32_t component,
                                       uint32_t offset,
                                       bool second_cycle)
{
    switch (source) {
    case RDP_COMBINER_COMBINED_RGB: return combined_in[component];
    case RDP_COMBINER_COMBINED_ALPHA: return combined_in[3];
    case RDP_COMBINER_TEXEL0_RGB: return load_extend_8(second_cycle
        ? &packet->texel1[component][offset] : &packet->texel0[component][offset]);
    case RDP_COMBINER_TEXEL0_ALPHA: return load_extend_8(second_cycle
        ? &packet->texel1[3][offset] : &packet->texel0[3][offset]);
    case RDP_COMBINER_TEXEL1_RGB: return load_extend_8(second_cycle
        ? &packet->texel0[component][offset] : &packet->texel1[component][offset]);
    case RDP_COMBINER_TEXEL1_ALPHA: return load_extend_8(second_cycle
        ? &packet->texel0[3][offset] : &packet->texel1[3][offset]);
    case RDP_COMBINER_SHADE_RGB: return load_extend_8(&packet->shade[component][offset]);
    case RDP_COMBINER_SHADE_ALPHA: return load_extend_8(&packet->shade[3][offset]);
    case RDP_COMBINER_PRIMITIVE_RGB: {
        uint32_t val = component == 0 ? state->primitive_color.r :
                       component == 1 ? state->primitive_color.g : state->primitive_color.b;
        return _mm256_set1_epi32(val);
    }
    case RDP_COMBINER_PRIMITIVE_ALPHA: return _mm256_set1_epi32(state->primitive_color.a);
    case RDP_COMBINER_ENVIRONMENT_RGB: {
        uint32_t val = component == 0 ? state->environment_color.r :
                       component == 1 ? state->environment_color.g : state->environment_color.b;
        return _mm256_set1_epi32(val);
    }
    case RDP_COMBINER_ENVIRONMENT_ALPHA: return _mm256_set1_epi32(state->environment_color.a);
    case RDP_COMBINER_KEY_CENTER: {
        const uint32_t val = component == 0 ? state->key_center.r :
                             component == 1 ? state->key_center.g : state->key_center.b;
        return _mm256_set1_epi32(val);
    }
    case RDP_COMBINER_KEY_SCALE: {
        const uint32_t val = component == 0 ? state->key_scale.r :
                             component == 1 ? state->key_scale.g : state->key_scale.b;
        return _mm256_set1_epi32(val);
    }
    case RDP_COMBINER_LOD_FRACTION: return load_extend_8(&packet->lod_fraction[offset]);
    case RDP_COMBINER_PRIMITIVE_LOD_FRACTION: return _mm256_set1_epi32(state->primitive_lod_fraction);
    case RDP_COMBINER_K4: return _mm256_set1_epi32(state->convert_k4);
    case RDP_COMBINER_K5: return _mm256_set1_epi32(state->convert_k5);
    case RDP_COMBINER_ONE: return _mm256_set1_epi32(0x100);
    default: return _mm256_setzero_si256();
    }
}

static inline __m256i simd_clamp_9(__m256i value)
{
    __m256i v = _mm256_and_si256(value, _mm256_set1_epi32(0x1ff));
    __m256i sel = _mm256_and_si256(_mm256_srli_epi32(v, 7), _mm256_set1_epi32(3));
    
    __m256i is_2 = _mm256_cmpeq_epi32(sel, _mm256_set1_epi32(2));
    __m256i is_3 = _mm256_cmpeq_epi32(sel, _mm256_set1_epi32(3));
    
    __m256i def = _mm256_and_si256(v, _mm256_set1_epi32(0xff));
    __m256i res = _mm256_blendv_epi8(def, _mm256_set1_epi32(0xff), is_2);
    return _mm256_blendv_epi8(res, _mm256_setzero_si256(), is_3);
}

static inline void simd_cycle(const rdp_combiner_cycle *cycle,
                             const rdp_color_pipeline_state *state,
                             const rdp_fragment_block *packet,
                             __m256i combined[4],
                             uint32_t offset,
                             bool second_cycle)
{
    __m256i combined_in[4];
    combined_in[0] = combined[0];
    combined_in[1] = combined[1];
    combined_in[2] = combined[2];
    combined_in[3] = combined[3];

    for (uint32_t component = 0; component < 4u; component++) {
        const bool alpha = component == 3u;
        __m256i a = simd_load_source(alpha ? cycle->alpha_a : cycle->rgb_a, packet, combined_in, state, component, offset, second_cycle);
        __m256i b = simd_load_source(alpha ? cycle->alpha_b : cycle->rgb_b, packet, combined_in, state, component, offset, second_cycle);
        __m256i c = simd_load_source(alpha ? cycle->alpha_c : cycle->rgb_c, packet, combined_in, state, component, offset, second_cycle);
        __m256i d = simd_load_source(alpha ? cycle->alpha_d : cycle->rgb_d, packet, combined_in, state, component, offset, second_cycle);
        
        a = simde_extend_9(a);
        b = simde_extend_9(b);
        c = simde_extend_9(c);
        d = simde_extend_9(d);
        
        __m256i diff = _mm256_sub_epi32(a, b);
        __m256i prod = _mm256_mullo_epi32(diff, c);
        __m256i d_scaled = _mm256_slli_epi32(d, 8);
        __m256i sum = _mm256_add_epi32(prod, d_scaled);
        sum = _mm256_add_epi32(sum, _mm256_set1_epi32(0x80));
        
        __m256i shifted = _mm256_srai_epi32(sum, 8);
        combined[component] = _mm256_and_si256(shifted, _mm256_set1_epi32(0x1ff));
    }
}

void rdp_combiner_evaluate_packet(const rdp_color_pipeline_state *state,
                                  rdp_fragment_block *packet)
{
    if (!state || !packet) return;
    
    uint32_t count = packet->count;
    uint32_t chunks = count >> 3;
    const uint32_t remainder = count & 7u;
    if (remainder >= 4u) {
        const uint32_t padded_end = (chunks + 1u) << 3;
        for (uint32_t lane = count; lane < padded_end; lane++) {
            for (uint32_t component = 0; component < 4u; component++) {
                packet->shade[component][lane] = 0u;
                packet->texel0[component][lane] = 0u;
                packet->texel1[component][lane] = 0u;
            }
            packet->lod_fraction[lane] = 0u;
        }
        chunks++;
    }
    uint16_t combined[4][RDP_PACKET_LANES] = {{0}};
    
    for (uint32_t chunk = 0; chunk < chunks; chunk++) {
        uint32_t offset = chunk << 3;
        
        __m256i simd_combined[4];
        simd_combined[0] = _mm256_setzero_si256();
        simd_combined[1] = _mm256_setzero_si256();
        simd_combined[2] = _mm256_setzero_si256();
        simd_combined[3] = _mm256_setzero_si256();
        
        if (state->two_cycle) {
            simd_cycle(&state->program.cycle[0], state, packet, simd_combined, offset, false);
            for (uint32_t component = 0; component < 4u; component++) {
                store_pack_8(&combined[component][offset], simd_combined[component]);
            }
        }
        
        for (uint32_t component = 0; component < 4u; component++) {
            simd_combined[component] = load_extend_8(&combined[component][offset]);
        }
        
        simd_cycle(&state->program.cycle[1], state, packet, simd_combined, offset,
                   state->two_cycle);
        
        for (uint32_t component = 0; component < 4u; component++) {
            __m256i clamped = simd_clamp_9(simd_combined[component]);
            store_pack_8(&packet->color[component][offset], clamped);
        }
    }

    /* A scalar tail avoids reading uninitialized lanes and lets callers build
     * only the live packet data instead of clearing the complete packet. */
    for (uint32_t lane = chunks << 3; lane < count; lane++) {
        rdp_combiner_inputs inputs = {
            .shade = { (uint8_t)packet->shade[0][lane], (uint8_t)packet->shade[1][lane],
                       (uint8_t)packet->shade[2][lane], (uint8_t)packet->shade[3][lane] },
            .texel0 = { (uint8_t)packet->texel0[0][lane], (uint8_t)packet->texel0[1][lane],
                        (uint8_t)packet->texel0[2][lane], (uint8_t)packet->texel0[3][lane] },
            .texel1 = { (uint8_t)packet->texel1[0][lane], (uint8_t)packet->texel1[1][lane],
                        (uint8_t)packet->texel1[2][lane], (uint8_t)packet->texel1[3][lane] },
            .primitive = state->primitive_color,
            .environment = state->environment_color,
            .lod_fraction = packet->lod_fraction[lane],
            .primitive_lod_fraction = state->primitive_lod_fraction,
            .k4 = (uint16_t)state->convert_k4,
            .k5 = (uint16_t)state->convert_k5
        };
        combiner_value combined_value = {0, 0, 0, 0};
        if (state->two_cycle) {
            evaluate_cycle(&state->program.cycle[0], &inputs, &combined_value);
            inputs.texel0 = (rdp_color){
                (uint8_t)packet->texel1[0][lane], (uint8_t)packet->texel1[1][lane],
                (uint8_t)packet->texel1[2][lane], (uint8_t)packet->texel1[3][lane] };
            inputs.texel1 = (rdp_color){
                (uint8_t)packet->texel0[0][lane], (uint8_t)packet->texel0[1][lane],
                (uint8_t)packet->texel0[2][lane], (uint8_t)packet->texel0[3][lane] };
        }
        evaluate_cycle(&state->program.cycle[1], &inputs, &combined_value);
        const rdp_color output = { clamp_9(combined_value.r), clamp_9(combined_value.g),
                                   clamp_9(combined_value.b), clamp_9(combined_value.a) };
        packet->color[0][lane] = output.r;
        packet->color[1][lane] = output.g;
        packet->color[2][lane] = output.b;
        packet->color[3][lane] = output.a;
    }
}
