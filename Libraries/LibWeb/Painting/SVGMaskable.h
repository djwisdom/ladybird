/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/PixelUnits.h>

#pragma once

namespace Web::Painting {

class SVGMaskable {
public:
    virtual ~SVGMaskable() = default;

    virtual GC::Ptr<DOM::Node const> dom_node_of_svg() const = 0;

    Optional<CSSPixelRect> get_masking_area_of_svg() const;
    Optional<Gfx::Bitmap::MaskKind> get_mask_type_of_svg() const;
    RefPtr<Gfx::ImmutableBitmap> calculate_mask_of_svg(DisplayListRecordingContext&, CSSPixelRect const& masking_area) const;
};

}
