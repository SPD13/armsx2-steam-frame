// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARMSX2 VR (GSVolumetric.h): the Vulkan device's multiview stereo and the depth, pre-HUD and
// right-eye images handed to the host with each libretro frame. Upstream GSDeviceVK.cpp only calls
// into these functions, so that merging upstream meets one-line hooks rather than this code.

#include "GS/GSVolumetric.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKLibretro.h"

#include "common/Console.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
	// The right eye's picture for each libretro backbuffer (which holds the left eye's), and the
	// ones displaced by a size change, kept until the host can no longer be reading them.
	constexpr int kBackbuffers = 3; // GSDeviceVK.cpp's kLibretroBackbuffers
	std::unique_ptr<GSTextureVK> s_bb_right[kBackbuffers];
	struct RetiredRight { std::unique_ptr<GSTextureVK> tex; u64 retired_at; };
	std::vector<RetiredRight> s_bb_right_retired;
	u64 s_right_presents = 0;
	constexpr u64 kRetireFrames = 6;
	constexpr VkClearValue s_clear_color = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
} // namespace

void GSDeviceVK::EnableStereoMultiview(VkDeviceCreateInfo& device_info, VkPhysicalDeviceMultiviewFeatures& multiview_feature)
{
	m_stereo_layers = 1;
	if (GSVolumetric::GetStereo().enabled && GSVolumetric::GetStereo().multiview)
	{
		VkPhysicalDeviceMultiviewFeatures probe_mv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
		VkPhysicalDeviceFeatures2 probe = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		Vulkan::AddPointerToChain(&probe, &probe_mv);
		vkGetPhysicalDeviceFeatures2(m_physical_device, &probe);
		if (probe_mv.multiview)
		{
			multiview_feature.multiview = VK_TRUE;
			Vulkan::AddPointerToChain(&device_info, &multiview_feature);
			m_stereo_layers = 2;
			Console.WriteLn("VK: multiview stereo enabled (2 layers per render target).");
		}
		else
		{
			Console.Warning("VK: multiview stereo requested, but the device has no multiview support.");
		}
	}
}

void GSDeviceVK::ReleaseStereoBackbuffers()
{
	for (std::unique_ptr<GSTextureVK>& bb : s_bb_right)
		bb.reset();
	s_bb_right_retired.clear();
	s_right_presents = 0;
}

GSTextureVK* GSDeviceVK::PresentRightEye(VkCommandBuffer cmdbuffer, GSTextureVK* bb, int bb_index)
{
	// GSVolumetric multiview stereo: the present drew the left eye (a layered texture is sampled as
	// its layer 0) into bb. The right eye is layer 1 of the same frame, scaled into a backbuffer of
	// its own through the same rectangles. Pause-menu repaints record no geometry: reuse the last.
	static PresentGeometry s_geometry;
	if (!m_present_geometry.src_rect.rempty())
		s_geometry = m_present_geometry;

	GSTextureVK* const current = static_cast<GSTextureVK*>(m_current);
	if (!current || current->GetArrayLayers() < 2 || s_geometry.src_rect.rempty())
		return nullptr;

	// Like the libretro backbuffers, a replaced one may still be read by the host for a few frames.
	s_right_presents++;
	std::erase_if(s_bb_right_retired, [](const RetiredRight& r) { return s_right_presents - r.retired_at > kRetireFrames; });

	if (bb_index < 0 || bb_index >= kBackbuffers)
		return nullptr;
	std::unique_ptr<GSTextureVK>& right = s_bb_right[bb_index];
	if (!right || right->GetWidth() != bb->GetWidth() || right->GetHeight() != bb->GetHeight())
	{
		if (right)
			s_bb_right_retired.push_back({std::move(right), s_right_presents});
		right = GSTextureVK::Create(GSTexture::RenderTarget, GSTexture::Format::Color, bb->GetWidth(), bb->GetHeight(), 1);
		if (!right)
			return nullptr;
	}

	right->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::TransferDst);
	const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
	vkCmdClearColorImage(cmdbuffer, right->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		&s_clear_color.color, 1, &range);

	// The draw rectangle can reach outside the backbuffer (zoom, overscan); a blit cannot, so clip
	// it and move the source edges with it.
	const GSVector4i& src = s_geometry.src_rect;
	const GSVector4& dst = s_geometry.draw_rect;
	const float cx0 = std::max(dst.x, 0.0f), cy0 = std::max(dst.y, 0.0f);
	const float cx1 = std::min(dst.z, static_cast<float>(bb->GetWidth()));
	const float cy1 = std::min(dst.w, static_cast<float>(bb->GetHeight()));
	if (cx1 > cx0 && cy1 > cy0 && dst.z > dst.x && dst.w > dst.y)
	{
		const float sx = static_cast<float>(src.z - src.x) / (dst.z - dst.x);
		const float sy = static_cast<float>(src.w - src.y) / (dst.w - dst.y);
		const GSVector4i s = GSVector4i(static_cast<int>(src.x + (cx0 - dst.x) * sx),
			static_cast<int>(src.y + (cy0 - dst.y) * sy), static_cast<int>(src.z - (dst.z - cx1) * sx),
			static_cast<int>(src.w - (dst.w - cy1) * sy)).rintersect(current->GetRect());
		const GSVector4i d(static_cast<int>(cx0), static_cast<int>(cy0), static_cast<int>(cx1), static_cast<int>(cy1));
		if (!s.rempty() && !d.rempty())
		{
			current->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::TransferSrc);
			const VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 1u}, {{s.x, s.y, 0}, {s.z, s.w, 1}},
				{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u}, {{d.x, d.y, 0}, {d.z, d.w, 1}}};
			vkCmdBlitImage(cmdbuffer, current->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, right->GetImage(),
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, s_geometry.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
		}
	}

	// The ImGui overlay (OSD messages) the present drew over the left eye, drawn again over the
	// right one, at the same place: it stays flat on the screen.
	right->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ColorAttachment);
	if (const VkFramebuffer fb = right->GetFramebuffer(false); fb != VK_NULL_HANDLE)
	{
		const u32 w = static_cast<u32>(right->GetWidth()), h = static_cast<u32>(right->GetHeight());
		const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
			GetRenderPass(right->GetVkFormat(), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD,
				VK_ATTACHMENT_STORE_OP_STORE),
			fb, {{0, 0}, {w, h}}, 0u, nullptr};
		vkCmdBeginRenderPass(cmdbuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
		const VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
		const VkRect2D scissor{{0, 0}, {w, h}};
		vkCmdSetViewport(cmdbuffer, 0, 1, &vp);
		vkCmdSetScissor(cmdbuffer, 0, 1, &scissor);
		m_scissor = GSVector4i(0, 0, static_cast<s32>(w), static_cast<s32>(h));
		m_dirty_flags |= DIRTY_UTILITY_STATE;
		DrawImGui();
		vkCmdEndRenderPass(cmdbuffer);
	}

	right->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ShaderReadOnly);
	return right.get();
}

GSTextureVK* GSDeviceVK::PrepareVolumetricPresent(VkCommandBuffer cmdbuffer, GSTextureVK* bb, int bb_index)
{
	// The host samples these images while the GS goes on: hand them over ready to be read.
	const GSVolumetricFrame& volumetric = GSVolumetric::CurrentFrame();
	GSTextureVK* depth = VKLibretro::Active ? static_cast<GSTextureVK*>(volumetric.depth) : nullptr;
	GSTextureVK* prehud = depth ? static_cast<GSTextureVK*>(volumetric.prehud) : nullptr;
	if (depth)
		depth->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ShaderReadOnly);
	if (prehud)
		prehud->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ShaderReadOnly);
	GSTextureVK* live_depth = depth ? static_cast<GSTextureVK*>(volumetric.live_depth) : nullptr;
	if (live_depth)
		live_depth->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ShaderReadOnly);
	GSTextureVK* bb_right = (VKLibretro::Active && IsMultiview()) ? PresentRightEye(cmdbuffer, bb, bb_index) : nullptr;
	return bb_right;
}

void GSDeviceVK::FillVolumetricFrame(VKLibretro::Frame& frame, GSTextureVK* bb, GSTextureVK* bb_right)
{
	const GSVolumetricFrame& volumetric = GSVolumetric::CurrentFrame();
	GSTextureVK* depth = VKLibretro::Active ? static_cast<GSTextureVK*>(volumetric.depth) : nullptr;
	GSTextureVK* prehud = depth ? static_cast<GSTextureVK*>(volumetric.prehud) : nullptr;
	GSTextureVK* live_depth = depth ? static_cast<GSTextureVK*>(volumetric.live_depth) : nullptr;
	frame.stereo_eye = volumetric.stereo_eye;
	frame.draw_rect[0] = volumetric.draw_rect.x, frame.draw_rect[1] = volumetric.draw_rect.y;
	frame.draw_rect[2] = volumetric.draw_rect.z, frame.draw_rect[3] = volumetric.draw_rect.w;
	if (bb_right)
	{
		frame.right_image = bb_right->GetImage();
		frame.right_view = bb_right->GetView();
	}
	if (depth)
	{
		frame.depth_image = depth->GetImage();
		frame.depth_view = depth->GetView();
		frame.depth_width = static_cast<u32>(depth->GetWidth());
		frame.depth_height = static_cast<u32>(depth->GetHeight());
		frame.depth_scale = volumetric.depth_scale;
		const GSVector4& uv = volumetric.src_uv;
		const GSVector4& rect = volumetric.draw_rect;
		frame.depth_src_uv[0] = uv.x, frame.depth_src_uv[1] = uv.y, frame.depth_src_uv[2] = uv.z, frame.depth_src_uv[3] = uv.w;
		frame.depth_draw_rect[0] = rect.x, frame.depth_draw_rect[1] = rect.y;
		frame.depth_draw_rect[2] = rect.z, frame.depth_draw_rect[3] = rect.w;
		if (prehud)
		{
			frame.prehud_image = prehud->GetImage();
			frame.prehud_view = prehud->GetView();
		}
		if (live_depth)
		{
			frame.live_depth_image = live_depth->GetImage();
			frame.live_depth_view = live_depth->GetView();
		}
	}
	if (const std::string prefix = GSVolumetric::TakePresentDumpRequest(); !prefix.empty())
	{
		bb->Save(prefix + "-left.png");
		if (bb_right)
			bb_right->Save(prefix + "-right.png");
		if (GSTexture* current = m_current)
		{
			current->GetLayerProxyTexture(0)->Save(prefix + "-layer0.png");
			if (current->GetArrayLayers() > 1)
				current->GetLayerProxyTexture(1)->Save(prefix + "-layer1.png");
		}
		Console.WriteLn("Debug: present dump %s (current %dx%d, %u layers)", prefix.c_str(),
			m_current ? m_current->GetWidth() : 0, m_current ? m_current->GetHeight() : 0,
			m_current ? m_current->GetArrayLayers() : 0u);
	}
}

void GSDeviceVK::CheckStereoRenderPass(VkRenderPass rp)
{
	// GSVolumetric multiview stereo: a per-eye target must be drawn by a multiview pass (its
	// framebuffer was made for one) and a single-layer one by a single-view pass. A mismatch is a
	// path that still needs a per-layer loop.
	if (!IsMultiview())
		return;
	const GSTextureVK* target = m_current_render_target ? m_current_render_target : m_current_depth_target;
	if (!target)
		return;
	const bool multiview_pass = m_multiview_render_passes.count(rp) != 0;
	const bool layered_target = target->GetArrayLayers() > 1;
	if (multiview_pass != layered_target)
	{
		static u32 s_reports = 0;
		if (s_reports++ < 20)
		{
			Console.Error("VK stereo: %s render pass on a %u-layer %dx%d target (format %u)",
				multiview_pass ? "multiview" : "single-view", target->GetArrayLayers(), target->GetWidth(),
				target->GetHeight(), static_cast<u32>(target->GetFormat()));
		}
	}
}

void GSDeviceVK::RestartMultiviewPassForNewConstants()
{
	// GSVolumetric multiview stereo: inside a multiview render pass, Turnip (Adreno 750, Mesa 26.3)
	// does not reliably apply a new dynamic offset of the constant buffers to the following draws;
	// they keep an earlier draw's constants. With the vertex transform taken from the wrong draw,
	// Time Crisis II's first shooting room came out with its walls missing and its furniture drawn
	// several times over (docs/m3-multiview.md). A pass begun after the change reads the right
	// offset, so restart the open pass in place: same framebuffer, contents loaded.
	if (!IsMultiview() || !InRenderPass())
		return;
	const VkRenderPass pass = m_current_render_pass;
	const GSVector4i area = m_current_render_pass_area;
	EndRenderPass();
	BeginRenderPass(GetRenderPassForRestarting(pass), area);
}

// Debugging: the $PS2V_FLUSH_FILE draw range (see DebugFlushAfterDraw).
static u32 s_debug_flush_first = 0;
static u32 s_debug_flush_last = 0;

void GSDeviceVK::DebugReadFlushRange()
{
	m_debug_draw_index = 0;
	static const char* s_file = std::getenv("PS2V_FLUSH_FILE");
	if (!s_file)
		return;
	u32 first = 0, last = 0;
	if (std::FILE* fp = std::fopen(s_file, "r"))
	{
		if (std::fscanf(fp, "%u %u", &first, &last) != 2)
			first = last = 0;
		std::fclose(fp);
	}
	if (first != s_debug_flush_first || last != s_debug_flush_last)
		Console.WriteLn("Debug: flushing after draws [%u, %u)", first, last);
	s_debug_flush_first = first;
	s_debug_flush_last = last;
}

void GSDeviceVK::DebugFlushAfterDraw()
{
	// Debugging (bisecting GPU ordering bugs, see docs/m3-multiview.md): after the hardware draws of a frame whose index is
	// in [first, last), end the pass and wait for the GPU. The range comes from the file
	// $PS2V_FLUSH_FILE ("first last"), read once per frame.
	if (s_debug_flush_first < s_debug_flush_last && m_debug_draw_index >= s_debug_flush_first &&
		m_debug_draw_index < s_debug_flush_last)
	{
		EndRenderPass();
		ExecuteCommandBuffer(WaitType::Sleep);
	}
	m_debug_draw_index++;
}
