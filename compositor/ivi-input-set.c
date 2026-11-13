/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ilm/ilm_common.h"
#include "ilm/ilm_control.h"
#include "ilm/ilm_input.h"

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <surface_id> | all\n", argv[0]);
        fprintf(stderr, "  <surface_id>: keyboard focus to one surface, pointer to both\n");
        fprintf(stderr, "  all:          keyboard and pointer focus to both surfaces\n");
        return 1;
    }
    int all = strcmp(argv[1], "all") == 0;
    t_ilm_surface target = all ? 0 : (t_ilm_surface)atoi(argv[1]);
    t_ilm_surface both[2] = { 1000, 2000 };

    if (ilm_init() != ILM_SUCCESS) {
        fprintf(stderr, "ilm_init() failed\n");
        return 1;
    }

    t_ilm_string seat = NULL;
    ilm_getDefaultSeat(&seat);
    if (!seat) seat = strdup("default");

    for (int i = 0; i < 2; i++)
        ilm_setInputAcceptanceOn(both[i], 1, &seat);

    ilm_setInputFocus(both, 2, ILM_INPUT_DEVICE_KEYBOARD, ILM_FALSE);
    if (all)
        ilm_setInputFocus(both, 2, ILM_INPUT_DEVICE_KEYBOARD, ILM_TRUE);
    else
        ilm_setInputFocus(&target, 1, ILM_INPUT_DEVICE_KEYBOARD, ILM_TRUE);

    for (int i = 0; i < 2; i++)
        ilm_setInputFocus(&both[i], 1,
            ILM_INPUT_DEVICE_POINTER | ILM_INPUT_DEVICE_TOUCH, ILM_TRUE);

    ilm_commitChanges();
    if (all)
        printf("Keyboard focus -> surfaces %u and %u\n", both[0], both[1]);
    else
        printf("Keyboard focus -> surface %u\n", target);
    ilm_destroy();
    free(seat);
    return 0;
}
