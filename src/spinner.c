/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * spinner.c -- Pixel hourglass, pure frozen data (SPEC.md section 8).
 *
 * A 7x7 card, 10 frames.  Alphabet: '#' glass, 's' sand, '|' thread
 * (middle column), '.' empty.  Sand only flows downward: frame 0 is
 * full on top, the last frames are full at the bottom; the total
 * number of sand grains (8) is constant across frames.  The CLI
 * prototype animates the frames while waiting for a result.
 */

#include "inference.h"

/* Glass outline shared by every frame:
 *   #######
 *   #.....#
 *   ##...##
 *   #..|..#
 *   ##...##
 *   #.....#
 *   #######
 * Top bulb holds 8 grains (row 1: 5 cells, row 2: 3 cells), drained
 * neck-first one grain per frame; the bottom bulb fills neck-first in
 * the same order.  Frame 9 holds the final state before looping. */
const char inf_spinner[INF_SPINNER_FRAMES][INF_SPINNER_SIZE]
                       [INF_SPINNER_SIZE + 1] =
{
    { "#######", "#sssss#", "##sss##", "#..|..#", "##...##", "#.....#", "#######" },
    { "#######", "#sssss#", "##.ss##", "#..|..#", "##s..##", "#.....#", "#######" },
    { "#######", "#sssss#", "##..s##", "#..|..#", "##ss.##", "#.....#", "#######" },
    { "#######", "#sssss#", "##...##", "#..|..#", "##sss##", "#.....#", "#######" },
    { "#######", "#.ssss#", "##...##", "#..|..#", "##sss##", "#s....#", "#######" },
    { "#######", "#..sss#", "##...##", "#..|..#", "##sss##", "#ss...#", "#######" },
    { "#######", "#...ss#", "##...##", "#..|..#", "##sss##", "#sss..#", "#######" },
    { "#######", "#....s#", "##...##", "#..|..#", "##sss##", "#ssss.#", "#######" },
    { "#######", "#.....#", "##...##", "#..|..#", "##sss##", "#sssss#", "#######" },
    { "#######", "#.....#", "##...##", "#..|..#", "##sss##", "#sssss#", "#######" },
};
