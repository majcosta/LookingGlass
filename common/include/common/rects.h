/**
 * Looking Glass
 * Copyright © 2017-2023 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef _LG_COMMON_RECTS_H_
#define _LG_COMMON_RECTS_H_

#include <stdint.h>
#include <string.h>

#include "common/framebuffer.h"
#include "common/types.h"

inline static void rectCopyUnaligned(uint8_t * dst, const uint8_t * src,
    int ystart, int yend, int dx, int dstPitch, int srcPitch, int width)
{
  src += ystart * srcPitch + dx;
  dst += ystart * dstPitch + dx;
  for (int i = ystart; i < yend; ++i)
  {
    memcpy(dst, src, width);
    src += srcPitch;
    dst += dstPitch;
 }
}

void rectsBufferToFramebuffer(FrameDamageRect * rects, int count, int bpp,
  FrameBuffer * frame, int dstPitch, int height,
  const uint8_t * src, int srcPitch);

void rectsFramebufferToBuffer(FrameDamageRect * rects, int count, int bpp,
  uint8_t * dst, int dstPitch, int height,
  const FrameBuffer * frame, int srcPitch);

int rectsMergeOverlapping(FrameDamageRect * rects, int count);
int rectsRejectContained(FrameDamageRect * rects, int count);

#endif
