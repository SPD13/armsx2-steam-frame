// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARMSX2 VR: depth that lines up with the presented image, for hosts that show the game in
// 3D (the Steam Frame VR host reads it through VKLibretro::Frame).
//
// The GS has no notion of "the depth of the displayed frame": the display circuits read colour
// targets, and a depth buffer is only tied to a colour target while they are drawn together. So
// the hardware renderer remembers which depth target each colour target's 3D geometry was drawn
// with, snapshots that depth (and the colour) when the frame is done - when drawing moves to
// another colour target, or before the next frame clears a shared Z buffer - and at vsync copies
// the displayed target's depth (the snapshot, or the live target if it is still current) into
// float canvases through the same rectangles the display merge uses. Depth values stay raw: GS Z /
// 2^32, larger is nearer. A HUD drawn with Z writes sits at the format's maximum.

#pragma once

#include "GS/GSVector.h"

#include <string>

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
	// Geometric stereo prototype (GSVolumetric::SetStereo): which eye the presented frame was drawn
	// for, 0 = left, 1 = right; -1 without stereo.
	int stereo_eye = -1;
	// Multiview stereo: the presented frame is a 2-layer texture, layer 0 for the left eye and 1 for
	// the right (stereo_eye is then -1).
	bool stereo_layers = false;
	// R32F canvas like depth: the depth target as it is at vsync, when depth comes from a snapshot
	// taken during this frame. A HUD drawn after it with Z writes shows here at the Z format's
	// maximum, which tells it apart from lights and effects drawn after it without Z.
	GSTexture* live_depth = nullptr;
	// The part of the merged frame (and so of the depth canvas) that is presented, as UVs ...
	GSVector4 src_uv = GSVector4::zero();
	// ... and where that part lands in the presented canvas, in pixels.
	GSVector4 draw_rect = GSVector4::zero();
};

namespace GSVolumetric
{
	// Off by default; a host that consumes depth turns it on before the GS opens, or while it runs
	// (depth then starts with the next frames drawn).
	void SetEnabled(bool enabled);
	bool IsEnabled();

	// Logs, every 300 vsyncs, which colour/depth targets the Z-writing draws used.
	void SetTrace(bool enabled);
	bool IsTraceEnabled();

	// Written on the GS thread during VSync, read by the present path of the same vsync.
	GSVolumetricFrame& CurrentFrame();

	// Geometric stereo, frame-alternating prototype: the eye flips with each new frame, and every
	// draw is shifted on screen by the current eye's offset from the screen centre times how far
	// behind the screen its geometry is - the parallax of looking through a window, from that eye:
	//   shift (NDC) = eye_offset (NDC) * depth_factor * f
	//   Q mode: f = max(0, 1 - convergence * Q)          (Q = 1/w of perspective vertices)
	//   Z mode: f = clamp(1 - Znorm / convergence, 0, 1) (Znorm = Z / format maximum)
	// f is 0 for depth at the convergence (on the screen) and 1 for the farthest geometry.
	// Screen-space (FST) draws and HUDs stamped at the maximum depth are not shifted.
	//
	// With multiview, both eyes are drawn in every frame instead: every render target gets one layer
	// per eye and every draw is a VK_KHR_multiview draw that applies each eye's shift to its own
	// layer. Needs the Vulkan renderer, and is fixed when the GS device is created.
	struct StereoSettings
	{
		bool enabled = false;
		bool z_mode = false;
		float convergence = 1.0f;
		bool multiview = false;
	};
	void SetStereo(const StereoSettings& settings);
	const StereoSettings& GetStereo();
	int CurrentEye(); // 0 left, 1 right
	// Per eye, set by the host every frame: its offset from the screen centre in NDC (x right, y
	// down, as the eye would see the screen move) and the depth factor R / (D + R) for geometry R
	// behind a screen D away.
	void SetEyeView(int eye, float offset_x, float offset_y, float depth_factor);
	// The eye's on-screen shift for the farthest geometry (offset times depth factor), in x and y.
	GSVector4 EyeShift(int eye);

	// Debugging: at the next present, write <prefix>-left.png / -right.png (the images handed to the
	// host) and <prefix>-layer0.png / -layer1.png (the frame's two layers). Any thread.
	void RequestPresentDump(std::string prefix);
	std::string TakePresentDumpRequest(); // the pending prefix, or empty
	// Debugging: log every hardware draw of the next `frames` frames.
	void ArmDrawTrace(int frames);
	bool DrawTraceActive();
	u32 DrawTraceNumber(); // which trace is running (counts from 1)

	// GSRenderer::VSync: before the display merge, and after it with the eye the displayed target
	// was drawn for (-1 if unknown), whether the frame repeats the last one, and whether the device
	// draws both eyes per frame.
	void BeginVSync();
	void EndMerge(int displayed_eye, bool idle_frame, bool multiview);
} // namespace GSVolumetric
