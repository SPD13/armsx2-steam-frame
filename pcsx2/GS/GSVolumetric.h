// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// PS2 Volumetric: depth that lines up with the presented image, for hosts that show the game in
// 3D (the Steam Frame VR host reads it through VKLibretro::Frame).
//
// The GS has no notion of "the depth of the displayed frame": the display circuits read colour
// targets, and a depth buffer is only tied to a colour target while they are drawn together. So
// the hardware renderer remembers which depth target each colour target's 3D geometry was drawn
// with, snapshots that depth (and the colour) when the frame is done - before the next frame clears
// a shared Z buffer, or a HUD is stamped into it - and at vsync copies the displayed target's
// snapshot into float canvases through the same rectangles the display merge uses. Depth values
// stay raw: GS Z / 2^32, larger is nearer.

#pragma once

#include "GS/GSVector.h"

class GSTexture;

struct GSVolumetricFrame
{
	// R32F (GSTexture::Format::DepthColor) canvas in the merged frame's space; 0 where nothing
	// with depth was displayed. Null when this vsync has no usable depth.
	GSTexture* depth = nullptr;
	// Multiplying a sample by this gives 0..1 of the Z buffer format's range (2^32 / Zmax).
	float depth_scale = 0.0f;
	// RGBA8 canvas like depth: the colour as it was when the depth was snapshotted. Pixels where the
	// presented frame differs were drawn after the 3D (HUD, overlays). Null when not available.
	GSTexture* prehud = nullptr;
	// The part of the merged frame (and so of the depth canvas) that is presented, as UVs ...
	GSVector4 src_uv = GSVector4::zero();
	// ... and where that part lands in the presented canvas, in pixels.
	GSVector4 draw_rect = GSVector4::zero();
};

namespace GSVolumetric
{
	// Off by default; a host that consumes depth turns it on before the GS opens.
	void SetEnabled(bool enabled);
	bool IsEnabled();

	// Logs, every 300 vsyncs, which colour/depth targets the Z-writing draws used.
	void SetTrace(bool enabled);
	bool IsTraceEnabled();

	// Written on the GS thread during VSync, read by the present path of the same vsync.
	GSVolumetricFrame& CurrentFrame();
} // namespace GSVolumetric
