#ifndef SR_HOST_H
#define SR_HOST_H

#include "sr_defs.h"

typedef enum sr_dp_register {
    SR_DP_START = 0,
    SR_DP_END,
    SR_DP_CURRENT,
    SR_DP_STATUS,
    SR_DP_CLOCK,
    SR_DP_BUFBUSY,
    SR_DP_PIPEBUSY,
    SR_DP_TMEM,
    SR_DP_REGISTER_COUNT
} sr_dp_register;

typedef enum sr_vi_register {
    SR_VI_STATUS = 0,
    SR_VI_ORIGIN,
    SR_VI_WIDTH,
    SR_VI_INTR,
    SR_VI_CURRENT,
    SR_VI_TIMING,
    SR_VI_V_SYNC,
    SR_VI_H_SYNC,
    SR_VI_LEAP,
    SR_VI_H_START,
    SR_VI_V_START,
    SR_VI_V_BURST,
    SR_VI_X_SCALE,
    SR_VI_Y_SCALE,
    SR_VI_REGISTER_COUNT
} sr_vi_register;

typedef struct sr_host_interface {
    uint8_t *rdram;
    uint32_t rdram_size;
    uint8_t *dmem;
    uint32_t *dp_regs[SR_DP_REGISTER_COUNT];
    uint32_t *vi_regs[SR_VI_REGISTER_COUNT];
    uint32_t *mi_intr_reg;
    void (*raise_mi_interrupt)(void *userdata);
    void *userdata;
} sr_host_interface;

#endif
