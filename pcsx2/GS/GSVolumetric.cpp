// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARMSX2 VR: the GSVolumetric state shared by the host, the renderer and the device (GSVolumetric.h).
// Kept out of the upstream GS files so that upstream merges only meet the calls into it.

#include "GS/GSVolumetric.h"

#include "common/Console.h"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

// Atomic: a host may switch it while the GS runs (the light gun needs depth in stereo mode).
static std::atomic<bool> s_volumetric_enabled{false};
static GSVolumetric::StereoSettings s_stereo;
static int s_stereo_eye = 0;

void GSVolumetric::SetStereo(const StereoSettings& settings)
{
	s_stereo = settings;
}

const GSVolumetric::StereoSettings& GSVolumetric::GetStereo()
{
	return s_stereo;
}

int GSVolumetric::CurrentEye()
{
	return s_stereo_eye;
}

// Written by the host thread, read by the GS thread per draw; a torn update only mixes two
// consecutive head positions.
static std::atomic<float> s_eye_view[2][4] = {};

void GSVolumetric::SetEyeView(int eye, float offset_x, float offset_y, float depth_factor, float distance_ratio)
{
	s_eye_view[eye][0].store(offset_x, std::memory_order_relaxed);
	s_eye_view[eye][1].store(offset_y, std::memory_order_relaxed);
	s_eye_view[eye][2].store(depth_factor, std::memory_order_relaxed);
	s_eye_view[eye][3].store(distance_ratio, std::memory_order_relaxed);
}

GSVector4 GSVolumetric::EyeShift(int eye)
{
	const float k = s_eye_view[eye][2].load(std::memory_order_relaxed);
	return GSVector4(s_eye_view[eye][0].load(std::memory_order_relaxed) * k,
		s_eye_view[eye][1].load(std::memory_order_relaxed) * k, s_eye_view[eye][3].load(std::memory_order_relaxed), 0.0f);
}

static std::mutex s_present_dump_mutex;
static std::string s_present_dump_prefix;

void GSVolumetric::RequestPresentDump(std::string prefix)
{
	std::lock_guard lock(s_present_dump_mutex);
	s_present_dump_prefix = std::move(prefix);
}

std::string GSVolumetric::TakePresentDumpRequest()
{
	std::lock_guard lock(s_present_dump_mutex);
	return std::exchange(s_present_dump_prefix, std::string());
}

static std::atomic<int> s_draw_trace_frames{0};
static std::atomic<int> s_draw_trace_pending{0};
static std::atomic<u32> s_draw_trace_number{0};

void GSVolumetric::ArmDrawTrace(int frames)
{
	s_draw_trace_pending.store(frames, std::memory_order_release);
}

u32 GSVolumetric::DrawTraceNumber()
{
	return s_draw_trace_number.load(std::memory_order_relaxed);
}

bool GSVolumetric::DrawTraceActive()
{
	return s_draw_trace_frames.load(std::memory_order_relaxed) > 0;
}

static bool s_volumetric_trace = false;
static GSVolumetricFrame s_volumetric_frame;

void GSVolumetric::SetTrace(bool enabled)
{
	s_volumetric_trace = enabled;
}

bool GSVolumetric::IsTraceEnabled()
{
	return s_volumetric_trace;
}

void GSVolumetric::SetEnabled(bool enabled)
{
	s_volumetric_enabled.store(enabled, std::memory_order_relaxed);
}

bool GSVolumetric::IsEnabled()
{
	return s_volumetric_enabled.load(std::memory_order_relaxed);
}

static std::atomic<bool> s_depth_extras{false};

void GSVolumetric::SetDepthExtras(bool enabled)
{
	s_depth_extras.store(enabled, std::memory_order_relaxed);
}

bool GSVolumetric::DepthExtras()
{
	return s_depth_extras.load(std::memory_order_relaxed);
}

GSVolumetricFrame& GSVolumetric::CurrentFrame()
{
	return s_volumetric_frame;
}

void GSVolumetric::BeginVSync()
{
	// Whatever the merge does not refresh must not be presented with this frame.
	s_volumetric_frame.depth = nullptr;
	s_volumetric_frame.prehud = nullptr;
	s_volumetric_frame.live_depth = nullptr;
}

void GSVolumetric::EndMerge(int displayed_eye, bool idle_frame, bool multiview)
{
	// Draw trace: the armed frames start at a frame boundary.
	if (const int pending = s_draw_trace_pending.exchange(0, std::memory_order_acq_rel); pending > 0)
	{
		s_draw_trace_frames.store(pending, std::memory_order_relaxed);
		s_draw_trace_number.fetch_add(1, std::memory_order_relaxed);
		Console.WriteLn("Draw trace: start (%d frames)", pending);
	}
	else if (s_draw_trace_frames.load(std::memory_order_relaxed) > 0 &&
			 s_draw_trace_frames.fetch_sub(1, std::memory_order_relaxed) == 1)
	{
		Console.WriteLn("Draw trace: end");
	}

	// Stereo: the presented frame belongs to the eye its target was drawn for (a double-buffered
	// game shows the buffer drawn during the previous frame); the next frame is drawn for the
	// other eye. With multiview the frame holds both eyes, one per layer.
	s_volumetric_frame.stereo_layers = s_stereo.enabled && multiview;
	if (s_stereo.enabled && !s_volumetric_frame.stereo_layers)
	{
		s_volumetric_frame.stereo_eye = displayed_eye >= 0 ? displayed_eye : s_stereo_eye;
		if (!idle_frame)
			s_stereo_eye ^= 1;
	}
	else
	{
		s_volumetric_frame.stereo_eye = -1;
	}
}
