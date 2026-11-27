#include "util/running_average.h"
#include <stdlib.h>

void running_average_init(running_average_t** ra, size_t window_size) {
    if (ra == NULL || window_size == 0)
        return;

    // calloc so index, sum and the sample buffer all start at zero.
    running_average_t* init = (running_average_t*)calloc(1, sizeof(running_average_t));

    if (init == NULL)
        return;

    init->buffer = (uint16_t*)calloc(window_size, sizeof(uint16_t));

    if (init->buffer == NULL) {
        free(init);
        return;
    }

    init->window = window_size;

    // Previously the allocated struct was never handed back to the caller, so the
    // average stayed NULL and every average-pressure reading (USB serial rows, SD
    // logs, get_pressure_average) was always 0.
    *ra = init;
}

void running_average_add_value(running_average_t* ra, uint16_t value) {
    if (ra == NULL || ra->buffer == NULL)
        return;

    if (!ra->primed) {
        // Fill the whole window with the first real sample. Starting from zeros made the
        // first readings after every boot ramp up from ~1/window of the real pressure, which
        // the app read as a huge pressure surge (pinned arousal at the start of a session).
        for (size_t i = 0; i < ra->window; i++) ra->buffer[i] = value;
        ra->sum = (uint32_t)value * ra->window;
        ra->index = 0;
        ra->averaged = value;
        ra->primed = 1;
        return;
    }

    ra->sum -= ra->buffer[ra->index];
    ra->buffer[ra->index] = value;
    ra->sum = ra->sum + value;
    ra->index = (ra->index + 1) % ra->window;
    ra->averaged = (uint16_t)(ra->sum / ra->window);
}

uint16_t running_avergae_get_average(running_average_t* ra) {
    if (ra == NULL)
        return 0;

    return ra->averaged;
}

void running_average_dispose(running_average_t* ra) {
    if (ra == NULL)
        return;

    free(ra->buffer);
    free(ra);
}
