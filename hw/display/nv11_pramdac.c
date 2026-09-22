/*
 * Nvidia Geforce2 MX400 (NV11B) PRAMDAC / PLL
 *
 * Copyright (c) 2026 Tisenu100
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "nv11.h"
#include "trace.h"

void nv11_pramdac_init(NV11State *s)
{
    memset(s->pramdac, 0, sizeof(s->pramdac));

    /* It is relatively unknown what status these registers have. X programs them even if we don't have a flat panel*/
    for (int head = 0; head < 2; head++) {
        s->pramdac[head][NV11_PRAMDAC_FP_VDISP / 4] = 4095;
        s->pramdac[head][NV11_PRAMDAC_FP_VT / 4] = 4095;
        s->pramdac[head][NV11_PRAMDAC_FP_HDISP / 4] = 4095;
        s->pramdac[head][NV11_PRAMDAC_FP_HT / 4] = 4095;
    }
    trace_nv11_pramdac_init();
}
