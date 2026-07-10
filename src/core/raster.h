#ifndef RASTER_H
#define RASTER_H

#include "rdp_commands.h"

sr_result raster_decode_triangle(const rdp_command *cmd, raster_decoded_triangle *out);
sr_result raster_submit_triangle(sr_memory *memory, tmem_state *tmem, const rdp_state *state, rdp_metrics *metrics, const rdp_command *cmd);
sr_result raster_submit_rectangle(sr_memory *memory, tmem_state *tmem, const rdp_state *state, rdp_metrics *metrics, const rdp_command *cmd);

#endif
