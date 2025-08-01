/*
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/Command.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

DisplayListRecorder::DisplayListRecorder(DisplayList& command_list)
    : m_command_list(command_list)
{
}

DisplayListRecorder::~DisplayListRecorder() = default;

#define APPEND(...)                                                        \
    do {                                                                   \
        Optional<i32> _scroll_frame_id;                                    \
        if (!m_scroll_frame_id_stack.is_empty())                           \
            _scroll_frame_id = m_scroll_frame_id_stack.last();             \
        RefPtr<ClipFrame const> _clip_frame;                               \
        if (!m_clip_frame_stack.is_empty())                                \
            _clip_frame = m_clip_frame_stack.last();                       \
        m_command_list.append(__VA_ARGS__, _scroll_frame_id, _clip_frame); \
    } while (false)

void DisplayListRecorder::paint_nested_display_list(RefPtr<DisplayList> display_list, Gfx::IntRect rect)
{
    APPEND(PaintNestedDisplayList { move(display_list), rect });
}

void DisplayListRecorder::add_rounded_rect_clip(CornerRadii corner_radii, Gfx::IntRect border_rect, CornerClip corner_clip)
{
    APPEND(AddRoundedRectClip { corner_radii, border_rect, corner_clip });
}

void DisplayListRecorder::add_mask(RefPtr<DisplayList> display_list, Gfx::IntRect rect)
{
    APPEND(AddMask { move(display_list), rect });
}

void DisplayListRecorder::fill_rect(Gfx::IntRect const& rect, Color color)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;
    APPEND(FillRect { rect, color });
}

void DisplayListRecorder::fill_path(FillPathUsingColorParams params)
{
    if (params.color.alpha() == 0)
        return;
    auto aa_translation = params.translation.value_or(Gfx::FloatPoint {});
    auto path_bounding_rect = params.path.bounding_box().translated(aa_translation);
    auto path_bounding_int_rect = enclosing_int_rect(path_bounding_rect);
    if (path_bounding_int_rect.is_empty())
        return;
    APPEND(FillPathUsingColor {
        .path_bounding_rect = path_bounding_int_rect,
        .path = move(params.path),
        .color = params.color,
        .winding_rule = params.winding_rule,
        .aa_translation = aa_translation,
    });
}

void DisplayListRecorder::fill_path(FillPathUsingPaintStyleParams params)
{
    auto aa_translation = params.translation.value_or(Gfx::FloatPoint {});
    auto path_bounding_rect = params.path.bounding_box().translated(aa_translation);
    auto path_bounding_int_rect = enclosing_int_rect(path_bounding_rect);
    if (path_bounding_int_rect.is_empty())
        return;
    APPEND(FillPathUsingPaintStyle {
        .path_bounding_rect = path_bounding_int_rect,
        .path = move(params.path),
        .paint_style = params.paint_style,
        .winding_rule = params.winding_rule,
        .opacity = params.opacity,
        .aa_translation = aa_translation,
    });
}

void DisplayListRecorder::stroke_path(StrokePathUsingColorParams params)
{
    if (params.color.alpha() == 0)
        return;
    auto aa_translation = params.translation.value_or(Gfx::FloatPoint {});
    auto path_bounding_rect = params.path.bounding_box().translated(aa_translation);
    // Increase path bounding box by `thickness` to account for stroke.
    path_bounding_rect.inflate(params.thickness, params.thickness);
    auto path_bounding_int_rect = enclosing_int_rect(path_bounding_rect);
    if (path_bounding_int_rect.is_empty())
        return;
    APPEND(StrokePathUsingColor {
        .cap_style = params.cap_style,
        .join_style = params.join_style,
        .miter_limit = params.miter_limit,
        .dash_array = move(params.dash_array),
        .dash_offset = params.dash_offset,
        .path_bounding_rect = path_bounding_int_rect,
        .path = move(params.path),
        .color = params.color,
        .thickness = params.thickness,
        .aa_translation = aa_translation,
    });
}

void DisplayListRecorder::stroke_path(StrokePathUsingPaintStyleParams params)
{
    auto aa_translation = params.translation.value_or(Gfx::FloatPoint {});
    auto path_bounding_rect = params.path.bounding_box().translated(aa_translation);
    // Increase path bounding box by `thickness` to account for stroke.
    path_bounding_rect.inflate(params.thickness, params.thickness);
    auto path_bounding_int_rect = enclosing_int_rect(path_bounding_rect);
    if (path_bounding_int_rect.is_empty())
        return;
    APPEND(StrokePathUsingPaintStyle {
        .cap_style = params.cap_style,
        .join_style = params.join_style,
        .miter_limit = params.miter_limit,
        .dash_array = move(params.dash_array),
        .dash_offset = params.dash_offset,
        .path_bounding_rect = path_bounding_int_rect,
        .path = move(params.path),
        .paint_style = params.paint_style,
        .thickness = params.thickness,
        .opacity = params.opacity,
        .aa_translation = aa_translation,
    });
}

void DisplayListRecorder::draw_ellipse(Gfx::IntRect const& a_rect, Color color, int thickness)
{
    if (a_rect.is_empty() || color.alpha() == 0)
        return;
    APPEND(DrawEllipse {
        .rect = a_rect,
        .color = color,
        .thickness = thickness,
    });
}

void DisplayListRecorder::fill_ellipse(Gfx::IntRect const& a_rect, Color color)
{
    if (a_rect.is_empty() || color.alpha() == 0)
        return;
    APPEND(FillEllipse { a_rect, color });
}

void DisplayListRecorder::fill_rect_with_linear_gradient(Gfx::IntRect const& gradient_rect, LinearGradientData const& data)
{
    if (gradient_rect.is_empty())
        return;
    APPEND(PaintLinearGradient { gradient_rect, data });
}

void DisplayListRecorder::fill_rect_with_conic_gradient(Gfx::IntRect const& rect, ConicGradientData const& data, Gfx::IntPoint const& position)
{
    if (rect.is_empty())
        return;
    APPEND(PaintConicGradient {
        .rect = rect,
        .conic_gradient_data = data,
        .position = position });
}

void DisplayListRecorder::fill_rect_with_radial_gradient(Gfx::IntRect const& rect, RadialGradientData const& data, Gfx::IntPoint center, Gfx::IntSize size)
{
    if (rect.is_empty())
        return;
    APPEND(PaintRadialGradient {
        .rect = rect,
        .radial_gradient_data = data,
        .center = center,
        .size = size });
}

void DisplayListRecorder::draw_rect(Gfx::IntRect const& rect, Color color, bool rough)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;
    APPEND(DrawRect {
        .rect = rect,
        .color = color,
        .rough = rough });
}

void DisplayListRecorder::draw_painting_surface(Gfx::IntRect const& dst_rect, NonnullRefPtr<Gfx::PaintingSurface> surface, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode)
{
    if (dst_rect.is_empty())
        return;
    APPEND(DrawPaintingSurface {
        .dst_rect = dst_rect,
        .surface = surface,
        .src_rect = src_rect,
        .scaling_mode = scaling_mode,
    });
}

void DisplayListRecorder::draw_scaled_immutable_bitmap(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, Gfx::ImmutableBitmap const& bitmap, Gfx::ScalingMode scaling_mode)
{
    if (dst_rect.is_empty())
        return;
    APPEND(DrawScaledImmutableBitmap {
        .dst_rect = dst_rect,
        .clip_rect = clip_rect,
        .bitmap = bitmap,
        .scaling_mode = scaling_mode,
    });
}

void DisplayListRecorder::draw_repeated_immutable_bitmap(Gfx::IntRect dst_rect, Gfx::IntRect clip_rect, NonnullRefPtr<Gfx::ImmutableBitmap const> bitmap, Gfx::ScalingMode scaling_mode, bool repeat_x, bool repeat_y)
{
    APPEND(DrawRepeatedImmutableBitmap {
        .dst_rect = dst_rect,
        .clip_rect = clip_rect,
        .bitmap = move(bitmap),
        .scaling_mode = scaling_mode,
        .repeat = { repeat_x, repeat_y },
    });
}

void DisplayListRecorder::draw_line(Gfx::IntPoint from, Gfx::IntPoint to, Color color, int thickness, Gfx::LineStyle style, Color alternate_color)
{
    if (color.alpha() == 0)
        return;
    APPEND(DrawLine {
        .color = color,
        .from = from,
        .to = to,
        .thickness = thickness,
        .style = style,
        .alternate_color = alternate_color,
    });
}

void DisplayListRecorder::draw_text(Gfx::IntRect const& rect, String raw_text, Gfx::Font const& font, Gfx::TextAlignment alignment, Color color)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;

    auto glyph_run = Gfx::shape_text({}, 0, raw_text.code_points(), font, Gfx::GlyphRun::TextType::Ltr, {});
    float baseline_x = 0;
    if (alignment == Gfx::TextAlignment::CenterLeft) {
        baseline_x = rect.x();
    } else if (alignment == Gfx::TextAlignment::Center) {
        baseline_x = static_cast<float>(rect.x()) + (static_cast<float>(rect.width()) - glyph_run->width()) / 2.0f;
    } else if (alignment == Gfx::TextAlignment::CenterRight) {
        baseline_x = static_cast<float>(rect.right()) - glyph_run->width();
    } else {
        // Unimplemented alignment.
        TODO();
    }
    auto metrics = font.pixel_metrics();
    float baseline_y = static_cast<float>(rect.y()) + metrics.ascent + (static_cast<float>(rect.height()) - (metrics.ascent + metrics.descent)) / 2.0f;
    draw_glyph_run({ baseline_x, baseline_y }, *glyph_run, color, rect, 1.0, Orientation::Horizontal);
}

void DisplayListRecorder::draw_glyph_run(Gfx::FloatPoint baseline_start, Gfx::GlyphRun const& glyph_run, Color color, Gfx::IntRect const& rect, double scale, Orientation orientation)
{
    if (color.alpha() == 0)
        return;
    APPEND(DrawGlyphRun {
        .glyph_run = glyph_run,
        .scale = scale,
        .rect = rect,
        .translation = baseline_start,
        .color = color,
        .orientation = orientation,
    });
}

void DisplayListRecorder::add_clip_rect(Gfx::IntRect const& rect)
{
    APPEND(AddClipRect { rect });
}

void DisplayListRecorder::translate(Gfx::IntPoint delta)
{
    APPEND(Translate { delta });
}

void DisplayListRecorder::save()
{
    ++m_save_nesting_level;
    APPEND(Save {});
}

void DisplayListRecorder::save_layer()
{
    ++m_save_nesting_level;
    APPEND(SaveLayer {});
}

void DisplayListRecorder::restore()
{
    --m_save_nesting_level;
    APPEND(Restore {});
}

void DisplayListRecorder::push_scroll_frame_id(Optional<i32> id)
{
    m_scroll_frame_id_stack.append(id);
}

void DisplayListRecorder::pop_scroll_frame_id()
{
    (void)m_scroll_frame_id_stack.take_last();
}

void DisplayListRecorder::push_clip_frame(RefPtr<ClipFrame const> clip_frame)
{
    m_clip_frame_stack.append(clip_frame);
}

void DisplayListRecorder::pop_clip_frame()
{
    (void)m_clip_frame_stack.take_last();
}

void DisplayListRecorder::push_stacking_context(PushStackingContextParams params)
{
    APPEND(PushStackingContext {
        .opacity = params.opacity,
        .compositing_and_blending_operator = params.compositing_and_blending_operator,
        .isolate = params.isolate,
        .transform = {
            .origin = params.transform.origin,
            .matrix = params.transform.matrix,
        },
        .clip_path = params.clip_path });
    m_clip_frame_stack.append({});
}

void DisplayListRecorder::pop_stacking_context()
{
    APPEND(PopStackingContext {});
    (void)m_clip_frame_stack.take_last();
}

void DisplayListRecorder::apply_backdrop_filter(Gfx::IntRect const& backdrop_region, BorderRadiiData const& border_radii_data, Gfx::Filter const& backdrop_filter)
{
    if (backdrop_region.is_empty())
        return;
    APPEND(ApplyBackdropFilter {
        .backdrop_region = backdrop_region,
        .border_radii_data = border_radii_data,
        .backdrop_filter = backdrop_filter,
    });
}

void DisplayListRecorder::paint_outer_box_shadow_params(PaintBoxShadowParams params)
{
    APPEND(PaintOuterBoxShadow { .box_shadow_params = params });
}

void DisplayListRecorder::paint_inner_box_shadow_params(PaintBoxShadowParams params)
{
    APPEND(PaintInnerBoxShadow { .box_shadow_params = params });
}

void DisplayListRecorder::paint_text_shadow(int blur_radius, Gfx::IntRect bounding_rect, Gfx::IntRect text_rect, Gfx::GlyphRun const& glyph_run, double glyph_run_scale, Color color, Gfx::FloatPoint draw_location)
{
    APPEND(PaintTextShadow {
        .glyph_run = glyph_run,
        .glyph_run_scale = glyph_run_scale,
        .shadow_bounding_rect = bounding_rect,
        .text_rect = text_rect,
        .draw_location = draw_location,
        .blur_radius = blur_radius,
        .color = color });
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color color, CornerRadius top_left_radius, CornerRadius top_right_radius, CornerRadius bottom_right_radius, CornerRadius bottom_left_radius)
{
    if (rect.is_empty())
        return;

    if (!top_left_radius && !top_right_radius && !bottom_right_radius && !bottom_left_radius) {
        fill_rect(rect, color);
        return;
    }

    APPEND(FillRectWithRoundedCorners {
        .rect = rect,
        .color = color,
        .corner_radii = {
            .top_left = top_left_radius,
            .top_right = top_right_radius,
            .bottom_right = bottom_right_radius,
            .bottom_left = bottom_left_radius,
        },
    });
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int radius)
{
    if (a_rect.is_empty() || color.alpha() == 0)
        return;
    fill_rect_with_rounded_corners(a_rect, color, radius, radius, radius, radius);
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius)
{
    if (a_rect.is_empty() || color.alpha() == 0)
        return;
    fill_rect_with_rounded_corners(a_rect, color,
        { top_left_radius, top_left_radius },
        { top_right_radius, top_right_radius },
        { bottom_right_radius, bottom_right_radius },
        { bottom_left_radius, bottom_left_radius });
}

void DisplayListRecorder::draw_triangle_wave(Gfx::IntPoint a_p1, Gfx::IntPoint a_p2, Color color, int amplitude, int thickness = 1)
{
    if (color.alpha() == 0)
        return;
    APPEND(DrawTriangleWave {
        .p1 = a_p1,
        .p2 = a_p2,
        .color = color,
        .amplitude = amplitude,
        .thickness = thickness });
}

void DisplayListRecorder::paint_scrollbar(int scroll_frame_id, Gfx::IntRect gutter_rect, Gfx::IntRect thumb_rect, CSSPixelFraction scroll_size, Color thumb_color, Color track_color, bool vertical)
{
    APPEND(PaintScrollBar {
        .scroll_frame_id = scroll_frame_id,
        .gutter_rect = gutter_rect,
        .thumb_rect = thumb_rect,
        .scroll_size = scroll_size,
        .thumb_color = thumb_color,
        .track_color = track_color,
        .vertical = vertical });
}

void DisplayListRecorder::apply_opacity(float opacity)
{
    // Implementation of this item does saveLayer(), so we need to increment the nesting level.
    ++m_save_nesting_level;
    APPEND(ApplyOpacity { .opacity = opacity });
}

void DisplayListRecorder::apply_compositing_and_blending_operator(Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    // Implementation of this item does saveLayer(), so we need to increment the nesting level.
    m_save_nesting_level++;
    APPEND(ApplyCompositeAndBlendingOperator { .compositing_and_blending_operator = compositing_and_blending_operator });
}

void DisplayListRecorder::apply_filter(Gfx::Filter filter)
{
    // Implementation of this item does saveLayer(), so we need to increment the nesting level.
    ++m_save_nesting_level;
    APPEND(ApplyFilter { .filter = move(filter) });
}

void DisplayListRecorder::apply_transform(Gfx::FloatPoint origin, Gfx::FloatMatrix4x4 matrix)
{
    APPEND(ApplyTransform {
        .origin = origin,
        .matrix = matrix,
    });
}

void DisplayListRecorder::apply_mask_bitmap(Gfx::IntPoint origin, Gfx::ImmutableBitmap const& bitmap, Gfx::Bitmap::MaskKind kind)
{
    APPEND(ApplyMaskBitmap {
        .origin = origin,
        .bitmap = bitmap,
        .kind = kind,
    });
}

}
